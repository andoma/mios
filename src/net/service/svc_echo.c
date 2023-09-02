#include <mios/service.h>
#include <malloc.h>
#include <stdlib.h>
#include <assert.h>

#include "net/pbuf.h"


typedef struct svc_echo {
  void *se_opaque;
  service_event_cb_t *se_cb;
  pbuf_t *se_pb;
} svc_echo_t;

static void *
echo_open(void *opaque, service_event_cb_t *cb,
          svc_pbuf_policy_t spp,
          service_get_flow_header_t *get_flow_hdr)
{
  svc_echo_t *se = xalloc(sizeof(svc_echo_t), 0, MEM_MAY_FAIL);
  if(se == NULL)
    return NULL;

  se->se_opaque = opaque;
  se->se_cb = cb;
  se->se_pb = NULL;
  return se;
}



static pbuf_t *
echo_push(void *opaque, struct pbuf *pb)
{
  svc_echo_t *se = opaque;
  assert(se->se_pb == NULL);
  se->se_pb = pb;
  return NULL;
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
  pbuf_t *pb = se->se_pb;
  se->se_pb = NULL;
  return pb;
}


static void
echo_close(void *opaque)
{
  svc_echo_t *se = opaque;
  se->se_cb(se->se_opaque, SERVICE_EVENT_CLOSE);
  pbuf_free(se->se_pb);
  free(se);
}

SERVICE_DEF("echo", 7, 7, SERVICE_TYPE_STREAM,
            echo_open, echo_push, echo_may_push, echo_pull, echo_close);
