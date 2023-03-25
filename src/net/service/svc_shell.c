#include <mios/suspend.h>
#include <mios/service.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/cli.h>

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

  pbuf_t *ss_txbuf;

  int ss_shutdown;

  size_t ss_max_fragment_size;

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

    if(ss->ss_txbuf != NULL)
      ss->ss_cb(ss->ss_opaque, SERVICE_EVENT_WAKEUP);

  } else {

    while(size) {

      if(ss->ss_txbuf == NULL) {
        ss->ss_txbuf = pbuf_make(0, 1);
      }

      size_t remain = ss->ss_max_fragment_size - ss->ss_txbuf->pb_buflen;
      if(remain == 0) {
        ss->ss_cb(ss->ss_opaque, SERVICE_EVENT_WAKEUP);
        cond_wait(&ss->ss_cond, &ss->ss_mutex);
        continue;
      }

      size_t to_copy = MIN(size, remain);
      memcpy(pbuf_append(ss->ss_txbuf, to_copy), buf, to_copy);
      buf += to_copy;
      size -= to_copy;
    }
  }
  mutex_unlock(&ss->ss_mutex);
}


__attribute__((noreturn))
static void *
shell_thread(void *arg)
{
  svc_shell_t *ss = arg;

  cli_on_stream(&ss->s, '>');

  ss->ss_cb(ss->ss_opaque, SERVICE_EVENT_CLOSE);

  mutex_lock(&ss->ss_mutex);
  while(!ss->ss_shutdown)
    cond_wait(&ss->ss_cond, &ss->ss_mutex);
  mutex_unlock(&ss->ss_mutex);

  pbuf_free(ss->ss_rxbuf);
  pbuf_free(ss->ss_txbuf);
  free(ss);

  wakelock_release();
  thread_exit(NULL);
}


static void *
shell_open(void *opaque, service_event_cb_t *cb, size_t max_fragment_size)
{
  svc_shell_t *ss = xalloc(sizeof(svc_shell_t), 0, MEM_MAY_FAIL);
  memset(ss, 0, sizeof(svc_shell_t));
  ss->ss_opaque = opaque;
  ss->ss_cb = cb;
  ss->s.read = svc_shell_read;
  ss->s.write = svc_shell_write;
  ss->ss_max_fragment_size = max_fragment_size;

  mutex_init(&ss->ss_mutex, "svc");
  cond_init(&ss->ss_cond, "svc");
  wakelock_acquire();
  error_t r = task_create_shell(shell_thread, ss, "remotecli");
  if(r) {
    free(ss);
    wakelock_release();
    ss = NULL;
  }
  return ss;
}

static struct pbuf *
shell_pull(void *opaque)
{
  svc_shell_t *ss = opaque;
  pbuf_t *pb;
  mutex_lock(&ss->ss_mutex);
  pb = ss->ss_txbuf;
  if(pb != NULL) {
    ss->ss_txbuf = NULL;
    cond_signal(&ss->ss_cond);
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


SERVICE_DEF("shell",
            shell_open, shell_push, shell_may_push, shell_pull, shell_close);
