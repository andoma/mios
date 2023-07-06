#include <mios/service.h>

#include <assert.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "net/pbuf.h"
#include "net/mbus/mbus.h"

#include "irq.h"

typedef struct mbus_svc {
  mbus_netif_t ms_mni;

  struct pbuf_queue ms_tx_queue;

  void *ms_opaque;
  service_event_cb_t *ms_cb;

} mbus_svc_t;


static void
mbus_svc_print_info(struct device *dev, struct stream *st)
{
  mbus_svc_t *ms = (mbus_svc_t *)dev;
  mbus_print_info(&ms->ms_mni, st);
}


static void
mbus_svc_dtor(struct device *dev)
{
  mbus_svc_t *ms = (mbus_svc_t *)dev;

  int q = irq_forbid(IRQ_LEVEL_NET);
  pbuf_free_queue_irq_blocked(&ms->ms_tx_queue);
  irq_permit(q);

  free(dev);
}


static const device_class_t mbus_svc_device_class = {
  .dc_print_info = mbus_svc_print_info,
  .dc_dtor = mbus_svc_dtor,
};


static pbuf_t *
ms_output(struct mbus_netif *mni, pbuf_t *pb)
{
  mbus_svc_t *ms = (mbus_svc_t *)mni;
  STAILQ_INSERT_TAIL(&ms->ms_tx_queue, pb, pb_link);
  mni->mni_tx_packets++;
  ms->ms_cb(ms->ms_opaque, SERVICE_EVENT_WAKEUP);
  return NULL;
}


static void *
ms_open(void *opaque, service_event_cb_t *cb,
        svc_pbuf_policy_t pbuf_policy,
        service_get_flow_header_t *get_flow_hdr)
{
  mbus_svc_t *ms = xalloc(sizeof(mbus_svc_t), 0, MEM_MAY_FAIL);
  if(ms == NULL)
    return NULL;

  memset(ms, 0, sizeof(mbus_svc_t));
  STAILQ_INIT(&ms->ms_tx_queue);

  ms->ms_mni.mni_output = ms_output;
  mbus_netif_attach(&ms->ms_mni, "svcmbus", &mbus_svc_device_class);

  ms->ms_opaque = opaque;
  ms->ms_cb = cb;

  return ms;
}


static int
ms_may_push(void *opaque)
{
  return 1;
}


static pbuf_t *
ms_pull(void *opaque)
{
  mbus_svc_t *ms = opaque;
  return pbuf_splice(&ms->ms_tx_queue);
}


static pbuf_t *
ms_push(void *opaque, struct pbuf *pb)
{
  mbus_svc_t *ms = opaque;
  STAILQ_INSERT_TAIL(&ms->ms_mni.mni_ni.ni_rx_queue, pb, pb_link);
  netif_wakeup(&ms->ms_mni.mni_ni);
  return NULL;
}


static void
ms_close(void *opaque)
{
  mbus_svc_t *ms = opaque;
  ms->ms_cb(ms->ms_opaque, SERVICE_EVENT_CLOSE);
  mbus_netif_detach(&ms->ms_mni);
}


SERVICE_DEF("mbus", 3, SERVICE_TYPE_DGRAM,
            ms_open, ms_push, ms_may_push, ms_pull, ms_close);
