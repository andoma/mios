#include <mios/suspend.h>
#include <mios/service.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/cli.h>
#include <mios/eventlog.h>

#include <sys/param.h>

#include <assert.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "net/pbuf.h"

// Maybe move socket_stream to a separate file
typedef struct socket_stream {

  stream_t ss_stream;

  socket_t *ss_sock;

  mutex_t ss_mutex;
  cond_t ss_cond;

  pbuf_t *ss_rxbuf;

  pbuf_t *ss_txbuf_head;
  pbuf_t *ss_txbuf_tail;

  uint8_t ss_shutdown;
  uint8_t ss_flushed;

} socket_stream_t;


static void
socket_stream_init(socket_stream_t *ss)
{
  mutex_init(&ss->ss_mutex, "sockstream");
  cond_init(&ss->ss_cond, "sockstream");
}

static int
socket_stream_read(struct stream *s, void *buf, size_t size, int wait)
{
  socket_stream_t *ss = (socket_stream_t *)s;

  int off = 0;
  mutex_lock(&ss->ss_mutex);

  while(1) {

    if(ss->ss_shutdown)
      break;

    if(ss->ss_rxbuf == NULL) {
      if(stream_wait_is_done(wait, off, size)) {
        break;
      }
      ss->ss_sock->net->event(ss->ss_sock->net_opaque, SOCKET_EVENT_PUSH);
      cond_wait(&ss->ss_cond, &ss->ss_mutex);
      continue;
    }

    const size_t avail = ss->ss_rxbuf->pb_pktlen;
    const size_t to_copy = MIN(avail, size - off);
    ss->ss_rxbuf = pbuf_read(ss->ss_rxbuf, buf + off, to_copy);

    off += to_copy;
    if(off == size)
      break;
  }
  mutex_unlock(&ss->ss_mutex);
  return off;
}


static void
socket_stream_write(struct stream *s, const void *buf, size_t size,
                    int flags)
{
  socket_stream_t *ss = (socket_stream_t *)s;

  mutex_lock(&ss->ss_mutex);

  if(buf == NULL) {
    // Flush

    if(ss->ss_txbuf_head != NULL) {
      ss->ss_flushed = 1;
      ss->ss_sock->net->event(ss->ss_sock->net_opaque, SOCKET_EVENT_PULL);
    }

  } else {

    while(size) {

      if(ss->ss_shutdown)
        break;

      if(ss->ss_txbuf_head == NULL) {
        ss->ss_txbuf_head = pbuf_make(ss->ss_sock->preferred_offset, 0);
        if(ss->ss_txbuf_head == NULL) {
          ss->ss_flushed = 1;
          ss->ss_sock->net->event(ss->ss_sock->net_opaque, SOCKET_EVENT_PULL);
          ss->ss_txbuf_head = pbuf_make(ss->ss_sock->preferred_offset, 1);
        }
        ss->ss_txbuf_tail = ss->ss_txbuf_head;
      }

      size_t remain = ss->ss_sock->max_fragment_size -
        ss->ss_txbuf_head->pb_pktlen;

      // Check if we need to allocate a new buffer for filling
      if(remain &&
         ss->ss_txbuf_tail->pb_buflen +
         ss->ss_txbuf_tail->pb_offset == PBUF_DATA_SIZE) {

        pbuf_t *pb = pbuf_make(0, 0);
        if(pb == NULL) {
          // Can't alloc, flush what we have
          remain = 0;
        } else {
          pb->pb_flags &= ~PBUF_SOP;
          ss->ss_txbuf_tail->pb_flags &= ~PBUF_EOP;
          ss->ss_txbuf_tail->pb_next = pb;
          ss->ss_txbuf_tail = pb;
        }

      }

      if(remain == 0) {
        ss->ss_flushed = 1;
        ss->ss_sock->net->event(ss->ss_sock->net_opaque, SOCKET_EVENT_PULL);
        cond_wait(&ss->ss_cond, &ss->ss_mutex);
        continue;
      }
      pbuf_t *pb = ss->ss_txbuf_tail;
      size_t to_copy = MIN(MIN(size, remain),
                           PBUF_DATA_SIZE - (pb->pb_offset + pb->pb_buflen));

      memcpy(pb->pb_data + pb->pb_offset + pb->pb_buflen, buf, to_copy);
      pb->pb_buflen += to_copy;
      ss->ss_txbuf_head->pb_pktlen += to_copy;
      buf += to_copy;
      size -= to_copy;
    }
  }
  mutex_unlock(&ss->ss_mutex);
}

static void
socket_stream_stop(socket_stream_t *ss)
{
  mutex_lock(&ss->ss_mutex);
  while(!ss->ss_shutdown)
    cond_wait(&ss->ss_cond, &ss->ss_mutex);
  mutex_unlock(&ss->ss_mutex);

  pbuf_free(ss->ss_rxbuf);
  pbuf_free(ss->ss_txbuf_head);
}


static struct pbuf *
socket_stream_pull(void *opaque)
{
  socket_stream_t *ss = opaque;
  pbuf_t *pb = NULL;
  mutex_lock(&ss->ss_mutex);
  if(ss->ss_flushed) {
    pb = ss->ss_txbuf_head;
    if(pb != NULL) {
      ss->ss_txbuf_head = NULL;
      ss->ss_txbuf_tail = NULL;
      cond_signal(&ss->ss_cond);
      ss->ss_flushed = 0;
    }
  }
  mutex_unlock(&ss->ss_mutex);
  return pb;
}


static uint32_t
socket_stream_push(void *opaque, struct pbuf *pb)
{
  socket_stream_t *ss = opaque;

  mutex_lock(&ss->ss_mutex);
  assert(ss->ss_rxbuf == NULL);
  ss->ss_rxbuf = pb;
  cond_signal(&ss->ss_cond);
  mutex_unlock(&ss->ss_mutex);
  return 0;
}

static int
socket_stream_may_push(void *opaque)
{
  socket_stream_t *ss = opaque;
  return ss->ss_rxbuf == NULL;
}


static void
socket_stream_close(void *opaque, const char *reason)
{
  socket_stream_t *ss = opaque;
  mutex_lock(&ss->ss_mutex);
  ss->ss_shutdown = 1;
  cond_signal(&ss->ss_cond);
  mutex_unlock(&ss->ss_mutex);
}


static const socket_app_fn_t socket_stream_fn = {
  .push = socket_stream_push,
  .may_push = socket_stream_may_push,
  .pull = socket_stream_pull,
  .close = socket_stream_close
};


typedef struct {
  socket_stream_t s;

#ifdef ENABLE_NET_IPV4
  uint8_t ss_iac_state;
  uint8_t ss_telnet_mode;
#endif

} svc_shell_t;


#ifdef ENABLE_NET_IPV4
static const uint8_t telnet_init[] = {
  255, 251, 1,  // WILL ECHO
  255, 251, 0,  // WILL BINARY
  255, 251, 3,  // WILL SUPRESS-GO-AHEAD
};
#endif

__attribute__((noreturn))
static void *
shell_thread(void *arg)
{
  svc_shell_t *ss = arg;

#ifdef ENABLE_NET_IPV4
  if(ss->ss_telnet_mode) {
    ss->s.ss_stream.write(&ss->s.ss_stream, telnet_init, sizeof(telnet_init), 0);
  }
#endif

  cli_on_stream(&ss->s.ss_stream, '>');

  socket_t *sk = ss->s.ss_sock;

  sk->net->event(sk->net_opaque, SOCKET_EVENT_CLOSE);

  socket_stream_stop(&ss->s);
  free(ss);

  wakelock_release();
  thread_exit(NULL);
}


#ifdef ENABLE_NET_IPV4
static int
telnet_read_filter(struct stream *s, void *buf, size_t size, int wait)
{
  svc_shell_t *ss = (svc_shell_t *)s;
  int r;
  while(1) {
    r = socket_stream_read(s, buf, size, wait);
    if(r > 0) {
      uint8_t *b = buf;
      size_t wrptr = 0;
      size_t cut = 0;
      for(size_t i = 0; i < r; i++) {
        uint8_t c = b[i];
        switch(ss->ss_iac_state) {
        case 0:
          if(c == 255) {
            cut++;
            ss->ss_iac_state = c;
          } else {
            if(c)
              b[wrptr++] = c;
            else
              cut++;
          }
          break;
        case 255:
          switch(c) {
          case 255:
            b[wrptr++] = c;
            break;
          case 240 ... 250:
            c = 0;
            // FALLTHRU
          default:
            ss->ss_iac_state = c;
            cut++;
            break;
          }
          break;
        default:
          cut++;
          ss->ss_iac_state = 0;
        }
      }
      r -= cut;
      if(r == 0 && wait) {
        continue;
      }
    }
    return r;
  }
}


static void
telnet_write_filter(struct stream *s, const void *buf, size_t size,
                    int flags)
{
  static const uint8_t crlf[2] = {0x0d, 0x0a};

  if(buf == NULL) {
    socket_stream_write(s, buf, size, flags);
  } else {
    const uint8_t *c = buf;
    size_t s0 = 0, i;
    for(i = 0; i < size; i++) {
      if(c[i] == 0x0a) {
        size_t len = i - s0;
        socket_stream_write(s, buf + s0, len, flags);
        socket_stream_write(s, crlf, 2, flags);
        s0 = i + 1;
      }
    }
    size_t len = i - s0;
    if(len) {
      socket_stream_write(s, buf + s0, len, flags);
    }
  }
}

#endif

static error_t
shell_open_raw(socket_t *s, int is_telnet)
{
  svc_shell_t *ss = xalloc(sizeof(svc_shell_t), 0, MEM_MAY_FAIL);
  if(ss == NULL)
    return ERR_NO_MEMORY;
  memset(ss, 0, sizeof(svc_shell_t));

  ss->s.ss_sock = s;
  ss->s.ss_sock->app = &socket_stream_fn;
  ss->s.ss_sock->app_opaque = ss;

  ss->s.ss_stream.read = socket_stream_read;
  ss->s.ss_stream.write = socket_stream_write;
#ifdef ENABLE_NET_IPV4
  if(is_telnet) {
    ss->s.ss_stream.read = telnet_read_filter;
    ss->s.ss_stream.write = telnet_write_filter;
  }
  ss->ss_telnet_mode = is_telnet;
#endif

  socket_stream_init(&ss->s);
  wakelock_acquire();
  error_t r = thread_create_shell(shell_thread, ss, "remotecli");
  if(r) {
    free(ss);
    wakelock_release();
    return r;
  }
  return 0;
}


static error_t
shell_open(socket_t *s)
{
  return shell_open_raw(s, 0);
}


SERVICE_DEF("shell", 0, 23, SERVICE_TYPE_STREAM, shell_open);

#ifdef ENABLE_NET_IPV4

static error_t
telnet_open(socket_t *s)
{
  return shell_open_raw(s, 1);
}

SERVICE_DEF("telnet", 23, 0, SERVICE_TYPE_STREAM, telnet_open);
#endif
