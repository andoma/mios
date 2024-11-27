#include <mios/service.h>
#include <malloc.h>
#include <stdlib.h>
#include <assert.h>

#include "net/pbuf.h"


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
echo_open(pushpull_t *s)
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

SERVICE_DEF("echo", 7, 7, SERVICE_TYPE_STREAM, echo_open);
