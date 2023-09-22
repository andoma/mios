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



typedef struct {
  stream_t s;

  void *ss_opaque;
  service_event_cb_t *ss_cb;

  mutex_t ss_mutex;
  cond_t ss_cond;

  pbuf_t *ss_rxbuf;

  pbuf_t *ss_txbuf_head;
  pbuf_t *ss_txbuf_tail;

  uint8_t ss_shutdown;
  uint8_t ss_flushed;
#ifdef ENABLE_NET_IPV4
  uint8_t ss_iac_state;
  uint8_t ss_telnet_mode;
#endif
  svc_pbuf_policy_t ss_pbuf_policy;

} svc_shell_t;


static int
svc_shell_read(struct stream *s, void *buf, size_t size, int wait)
{
  svc_shell_t *ss = (svc_shell_t *)s;

  int off = 0;
  mutex_lock(&ss->ss_mutex);

  while(1) {

    if(ss->ss_shutdown)
      break;

    if(ss->ss_rxbuf == NULL) {
      if(stream_wait_is_done(wait, off, size)) {
        break;
      }
      ss->ss_cb(ss->ss_opaque, SERVICE_EVENT_WAKEUP);
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
svc_shell_write(struct stream *s, const void *buf, size_t size)
{
  svc_shell_t *ss = (svc_shell_t *)s;

  mutex_lock(&ss->ss_mutex);

  if(buf == NULL) {
    // Flush

    if(ss->ss_txbuf_head != NULL) {
      ss->ss_flushed = 1;
      ss->ss_cb(ss->ss_opaque, SERVICE_EVENT_WAKEUP);
    }

  } else {

    while(size) {

      if(ss->ss_shutdown)
        break;

      if(ss->ss_txbuf_head == NULL) {
        ss->ss_txbuf_head = pbuf_make(ss->ss_pbuf_policy.preferred_offset, 0);
        if(ss->ss_txbuf_head == NULL) {
          ss->ss_flushed = 1;
          ss->ss_cb(ss->ss_opaque, SERVICE_EVENT_WAKEUP);
          ss->ss_txbuf_head = pbuf_make(ss->ss_pbuf_policy.preferred_offset, 1);
        }
        ss->ss_txbuf_tail = ss->ss_txbuf_head;
      }

      size_t remain = ss->ss_pbuf_policy.max_fragment_size -
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
        ss->ss_cb(ss->ss_opaque, SERVICE_EVENT_WAKEUP);
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
    ss->s.write(&ss->s, telnet_init, sizeof(telnet_init));
  }
#endif

  cli_on_stream(&ss->s, '>');

  ss->ss_cb(ss->ss_opaque, SERVICE_EVENT_CLOSE);

  mutex_lock(&ss->ss_mutex);
  while(!ss->ss_shutdown)
    cond_wait(&ss->ss_cond, &ss->ss_mutex);
  mutex_unlock(&ss->ss_mutex);

  pbuf_free(ss->ss_rxbuf);
  pbuf_free(ss->ss_txbuf_head);
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
    r = svc_shell_read(s, buf, size, wait);
    if(r > 0) {
      uint8_t *b = buf;

      size_t cut = 0;
      size_t wrptr = 0;
      for(size_t i = 0; i < r; i++) {
        uint8_t c = b[i];
        switch(ss->ss_iac_state) {
        case 0:
          if(c == 255) {
            cut++;
            ss->ss_iac_state = c;
          } else {
            b[wrptr++] = c;
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
telnet_write_filter(struct stream *s, const void *buf, size_t size)
{
  static const uint8_t crlf[2] = {0x0d, 0x0a};

  if(buf == NULL) {
    svc_shell_write(s, buf, size);
  } else {
    const uint8_t *c = buf;
    size_t s0 = 0, i;
    for(i = 0; i < size; i++) {
      if(c[i] == 0x0a) {
        size_t len = i - s0;
        svc_shell_write(s, buf + s0, len);
        svc_shell_write(s, crlf, 2);
        s0 = i + 1;
      }
    }
    size_t len = i - s0;
    if(len) {
      svc_shell_write(s, buf + s0, len);
    }
  }
}

#endif

static void *
shell_open_raw(void *opaque, service_event_cb_t *cb,
               svc_pbuf_policy_t pbuf_policy,
               service_get_flow_header_t *get_flow_hdr,
               int telnet)
{
  svc_shell_t *ss = xalloc(sizeof(svc_shell_t), 0, MEM_MAY_FAIL);
  if(ss == NULL)
    return NULL;
  memset(ss, 0, sizeof(svc_shell_t));
  ss->ss_opaque = opaque;
  ss->ss_cb = cb;
  ss->s.read = svc_shell_read;
  ss->s.write = svc_shell_write;
#ifdef ENABLE_NET_IPV4
  if(telnet) {
    ss->s.read = telnet_read_filter;
    ss->s.write = telnet_write_filter;
  }
  ss->ss_telnet_mode = telnet;
#endif
  ss->ss_pbuf_policy = pbuf_policy;

  mutex_init(&ss->ss_mutex, "svc");
  cond_init(&ss->ss_cond, "svc");
  wakelock_acquire();
  error_t r = task_create_shell(shell_thread, ss, "remotecli", 0);
  if(r) {
    free(ss);
    wakelock_release();
    ss = NULL;
  }
  return ss;
}


static void *
shell_open(void *opaque, service_event_cb_t *cb,
           svc_pbuf_policy_t pbuf_policy,
           service_get_flow_header_t *get_flow_hdr)
{
  return shell_open_raw(opaque, cb, pbuf_policy, get_flow_hdr, 0);
}



static void *
telnet_open(void *opaque, service_event_cb_t *cb,
           svc_pbuf_policy_t pbuf_policy,
           service_get_flow_header_t *get_flow_hdr)
{
  return shell_open_raw(opaque, cb, pbuf_policy, get_flow_hdr, 1);
}



static struct pbuf *
shell_pull(void *opaque)
{
  svc_shell_t *ss = opaque;
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


static pbuf_t *
shell_push(void *opaque, struct pbuf *pb)
{
  svc_shell_t *ss = opaque;

  mutex_lock(&ss->ss_mutex);
  assert(ss->ss_rxbuf == NULL);
  ss->ss_rxbuf = pb;
  cond_signal(&ss->ss_cond);
  mutex_unlock(&ss->ss_mutex);
  return NULL;
}

static int
shell_may_push(void *opaque)
{
  svc_shell_t *ss = opaque;
  return ss->ss_rxbuf == NULL;
}


static void
shell_close(void *opaque)
{
  svc_shell_t *ss = opaque;
  mutex_lock(&ss->ss_mutex);
  ss->ss_shutdown = 1;
  cond_signal(&ss->ss_cond);
  mutex_unlock(&ss->ss_mutex);
}


SERVICE_DEF("shell", 0, 23, SERVICE_TYPE_STREAM,
            shell_open, shell_push, shell_may_push, shell_pull, shell_close);

#ifdef ENABLE_NET_IPV4
SERVICE_DEF("telnet", 23, 0, SERVICE_TYPE_STREAM,
            telnet_open, shell_push, shell_may_push, shell_pull, shell_close);
#endif
