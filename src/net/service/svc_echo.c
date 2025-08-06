#include <mios/service.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <malloc.h>

#include "net/pbuf.h"

__attribute__((noreturn))
static void *
echo_thread(void *arg)
{
  stream_t *s = arg;
  char buf[64];
  while(1) {
    ssize_t r;
    r = stream_read(s, buf, sizeof(buf), 0);
    if(r == 0) {
      stream_write(s, NULL, 0, 0);
      r = stream_read(s, buf, sizeof(buf), 1);
    }
    if(r < 0)
      break;
    if(r > 0) {
      stream_write(s, buf, r, 0);
    }
  }
  stream_close(s);
  thread_exit(NULL);
}

static error_t
echo_open_stream(stream_t *s)
{
  thread_t *t = thread_create(echo_thread, s, 0, "echo", TASK_DETACHED, 5);
  if(t)
    return 0;
  return ERR_NO_MEMORY;
}





typedef struct svc_echo {
  pbuf_t *se_pb;
  pushpull_t *se_sock;
} svc_echo_t;

static uint32_t
echo_push(void *opaque, struct pbuf *pb)
{
  svc_echo_t *se = opaque;
  assert(se->se_pb == NULL);
  se->se_pb = pb;
  return 0;
}

static int
echo_may_push(void *opaque)
{
  svc_echo_t *se = opaque;
  return se->se_pb == NULL;
}


static pbuf_t *
echo_pull(void *opaque)
{
  svc_echo_t *se = opaque;
  pushpull_t *s = se->se_sock;
  pbuf_t *pb = se->se_pb;
  se->se_pb = NULL;
  s->net->event(s->net_opaque, PUSHPULL_EVENT_PUSH);
  return pb;
}


static void
echo_close(void *opaque, const char *reason)
{
  svc_echo_t *se = opaque;
  pushpull_t *s = se->se_sock;
  s->net->event(s->net_opaque, PUSHPULL_EVENT_CLOSE);
  pbuf_free(se->se_pb);
  free(se);
}

static const pushpull_app_fn_t echo_fn = {
  .push = echo_push,
  .may_push = echo_may_push,
  .pull = echo_pull,
  .close = echo_close
};

static error_t
echo_open_pushpull(pushpull_t *s)
{
  svc_echo_t *se = xalloc(sizeof(svc_echo_t), 0, MEM_MAY_FAIL);
  if(se == NULL)
    return ERR_NO_MEMORY;

  se->se_pb = NULL;
  se->se_sock = s;
  s->app = &echo_fn;
  s->app_opaque = se;
  return 0;
}

static const service_t svc_echo
__attribute__ ((used, section("servicedef"))) = {
  .name = "echo",
  .ip_port = 7,
  .open_stream = echo_open_stream,
  .open_pushpull = echo_open_pushpull,
};
