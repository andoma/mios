#include <mios/service.h>
#include <malloc.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <mios/eventlog.h>

#include "net/pbuf.h"


typedef struct svc_discard {
  void *se_opaque;
  service_event_cb_t *se_cb;
} svc_discard_t;

static void *
discard_open(void *opaque, service_event_cb_t *cb,
          svc_pbuf_policy_t spp,
          service_get_flow_header_t *get_flow_hdr)
{
  svc_discard_t *se = xalloc(sizeof(svc_discard_t), 0, MEM_MAY_FAIL);
  if(se == NULL)
    return NULL;

  se->se_opaque = opaque;
  se->se_cb = cb;
  return se;
}



static pbuf_t *
discard_push(void *opaque, struct pbuf *pb)
{
  int id = -1;
  if(pb->pb_buflen >= 4)
    memcpy(&id, pbuf_cdata(pb, 0), 4);
  //  evlog(LOG_DEBUG, "Disc-0x%x %d bytes", id, pb->pb_pktlen);
  return pb;
}

static int
discard_may_push(void *opaque)
{
  return 1;
}


static pbuf_t *
discard_pull(void *opaque)
{
  return NULL;
}


static void
discard_close(void *opaque)
{
  svc_discard_t *se = opaque;
  se->se_cb(se->se_opaque, SERVICE_EVENT_CLOSE);
  free(se);
}

SERVICE_DEF("discard", 8, SERVICE_TYPE_DGRAM,
            discard_open, discard_push, discard_may_push,
            discard_pull, discard_close);
