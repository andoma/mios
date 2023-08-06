// rename to mbus_gateway.c

#include <mios/service.h>
#include <mios/eventlog.h>

#include <sys/param.h>

#include <assert.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "net/pbuf.h"
#include "net/mbus/mbus.h"

#include "irq.h"

#include "mbus_seqpkt_defs.h"

/* flow tag format:
   0-15   flowid
   16-23  network side address
   24-31  service side address
*/

typedef struct mbus_svc {
  mbus_netif_t ms_mni;

  struct pbuf_queue ms_tx_queue;

  void *ms_opaque;
  service_event_cb_t *ms_cb;

  struct mbus_flow_list ms_flows;

} mbus_svc_t;


static pbuf_t *
mbus_seqpkt_inter_flow_xmit(pbuf_t *pb, mbus_seqpkt_con_t *msc)
{
  mbus_svc_t *ms = msc->msc_app_opaque;
  if(ms == NULL)
    return pb;

  uint16_t flow = msc->msc_flow.mf_tag;
  uint8_t src_addr = msc->msc_flow.mf_tag >> 24;
  uint8_t dst_addr = msc->msc_flow.mf_tag >> 16;

  pb = pbuf_prepend(pb, 3, 0, 0);
  if(pb == NULL)
    return NULL;
  uint8_t *hdr = pbuf_data(pb, 0);
  hdr[0] = dst_addr;
  hdr[1] = src_addr | ((flow >> 3) & 0x60);
  hdr[2] = flow;

  mbus_append_crc(pb);

  STAILQ_INSERT_TAIL(&ms->ms_mni.mni_ni.ni_rx_queue, pb, pb_link);
  netif_wakeup(&ms->ms_mni.mni_ni);
  return NULL;
}

static uint8_t
mbus_seqpkt_inter_update_cts(mbus_seqpkt_con_t *msc)
{
  return 0;
}

static pbuf_t *
mbus_seqpkt_inter_recv(pbuf_t *pb, mbus_seqpkt_con_t *msc)
{
  mbus_svc_t *ms = msc->msc_app_opaque;
  if(ms == NULL)
    return pb;

  uint16_t flow = msc->msc_flow.mf_tag;
  uint8_t dst_addr = msc->msc_flow.mf_tag >> 24;
  uint8_t src_addr = msc->msc_flow.mf_tag >> 16;

  assert(pb->pb_next == NULL);

  pb = pbuf_prepend(pb, 4, 0, 0);
  if(pb == NULL)
    return NULL;

  int creds = msc->msc_remote_avail_credits;
  creds = MIN(creds, 15);

  uint8_t *hdr = pbuf_data(pb, 0);
  hdr[0] = dst_addr;
  hdr[1] = src_addr | ((flow >> 3) & 0x60);
  hdr[2] = flow;
  hdr[3] = (pb->pb_flags & 3) | (creds << 2);

  mbus_append_crc(pb);

  STAILQ_INSERT_TAIL(&ms->ms_tx_queue, pb, pb_link);
  ms->ms_cb(ms->ms_opaque, SERVICE_EVENT_WAKEUP);

  msc->msc_remote_avail_credits -= creds;
  msc->msc_remote_xmit_credits += creds;

  return NULL;
}


static int
give_credits(mbus_seqpkt_con_t *msc, int num_credits)
{
  assert(num_credits < 16);

  mbus_svc_t *ms = msc->msc_app_opaque;
  if(ms == NULL)
    return 1;

  pbuf_t *pb = pbuf_make(4, 0);
  if(pb == NULL)
    return 1;

  uint16_t flow = msc->msc_flow.mf_tag;
  uint8_t dst_addr = msc->msc_flow.mf_tag >> 24;
  uint8_t src_addr = msc->msc_flow.mf_tag >> 16;

  uint8_t *hdr = pbuf_append(pb, 4);
  hdr[0] = dst_addr;
  hdr[1] = src_addr | ((flow >> 3) & 0x60);
  hdr[2] = flow;
  hdr[3] = num_credits << 2;

  mbus_append_crc(pb);

  STAILQ_INSERT_TAIL(&ms->ms_tx_queue, pb, pb_link);
  ms->ms_cb(ms->ms_opaque, SERVICE_EVENT_WAKEUP);

  msc->msc_remote_xmit_credits += num_credits;
  return 0;
}

static void
mbus_seqpkt_inter_post_send(mbus_seqpkt_con_t *msc)
{
  // Credits we can give because we've processed these packets
  int creds = msc->msc_remote_avail_credits;

  creds = MIN(creds, 15);
  if(creds < 8)
    return;

  if(give_credits(msc, creds))
    return;
  msc->msc_remote_avail_credits -= creds;
}


static void
mbus_seqpkt_inter_svc_shut(mbus_seqpkt_con_t *msc, const char *reason)
{
  mbus_svc_t *ms = msc->msc_app_opaque;
  evlog(LOG_DEBUG, "%s: %s ms=%p", __FUNCTION__, reason, ms);
  if(ms == NULL)
    return;

  uint16_t flow = msc->msc_flow.mf_tag;
  uint8_t dst_addr = msc->msc_flow.mf_tag >> 24;
  uint8_t src_addr = msc->msc_flow.mf_tag >> 16;

  pbuf_t *pb = pbuf_make(4, 1);

  uint8_t *hdr = pbuf_append(pb, 4);
  hdr[0] = dst_addr;
  hdr[1] = src_addr | ((flow >> 3) & 0x60);
  hdr[2] = flow;
  hdr[3] = SP_EOS;

  mbus_append_crc(pb);

  STAILQ_INSERT_TAIL(&ms->ms_tx_queue, pb, pb_link);
  ms->ms_cb(ms->ms_opaque, SERVICE_EVENT_WAKEUP);
  msc->msc_app_opaque = NULL;
}

static mbus_seqpkt_con_t *
flow_find(mbus_svc_t *ms, uint32_t tag, int create)
{
  mbus_flow_t *mf;

  LIST_FOREACH(mf, &ms->ms_flows, mf_link) {
    if(mf->mf_tag == tag)
      return (mbus_seqpkt_con_t *)mf;
  }
  if(!create)
    return NULL;

  mbus_seqpkt_con_t *msc = xalloc(sizeof(mbus_seqpkt_con_t), 0, MEM_MAY_FAIL);
  if(msc == NULL)
    return NULL;
  memset(msc, 0, sizeof(mbus_seqpkt_con_t));
  msc->msc_name = "gdproxy";
  msc->msc_remote_xmit_credits = 1;

  LIST_INSERT_HEAD(&ms->ms_flows, &msc->msc_flow, mf_link);
  msc->msc_flow.mf_tag = tag;

  mbus_seqpkt_con_init(msc);
  msc->msc_local_flags = SP_CTS;

  msc->msc_xmit = mbus_seqpkt_inter_flow_xmit;

  msc->msc_update_local_cts = mbus_seqpkt_inter_update_cts;
  msc->msc_recv = mbus_seqpkt_inter_recv;
  msc->msc_shut_app = mbus_seqpkt_inter_svc_shut;
  msc->msc_post_send = mbus_seqpkt_inter_post_send;

  msc->msc_app_opaque = ms;

  return msc;
}

static void
ms_remove_all_flows(mbus_svc_t *ms)
{
  mbus_flow_t *mf;
  while((mf = LIST_FIRST(&ms->ms_flows)) != NULL) {
    mbus_seqpkt_con_t *msc = (mbus_seqpkt_con_t *)mf;
    if(msc->msc_app_opaque)
      net_task_raise(&msc->msc_task, SERVICE_EVENT_CLOSE);
    mbus_flow_remove(mf);
  }
}

static pbuf_t *
ms_inspect_from_svc(mbus_svc_t *ms, pbuf_t *pb)
{
  if(mbus_crc32(pb, 0)) {
    panic("%s: CRC ERROR", __FUNCTION__);
  }

  if(pbuf_pullup(pb, 4)) {
    panic("%s: Pullup failed", __FUNCTION__);
  }
  uint8_t *pkt = pbuf_data(pb, 0);
  const uint8_t dst_addr = pkt[0] & 0x3f;
  if(dst_addr >= 32)
    return pb; // Multicast

  const uint16_t flow = pkt[2] | ((pkt[1] << 3) & 0x300);
  const uint8_t src_addr = pkt[1] & 0x1f;
  int create = pkt[1] & 0x80;

  const uint32_t tag = flow | (src_addr << 24) | (dst_addr << 16);
  if(create) {
    // Only create flows for SEQPKT Guaranteed Delivery Mode
    if(pkt[3] != 3)
      create = 0;
  }
  mbus_seqpkt_con_t *msc = flow_find(ms, tag, create);
  if(msc == NULL)
    return pb;

  pbuf_trim(pb, 4); // Drop CRC

  if(create) {
    pkt[3] = 1; // Transform into regular SEQPKT flow
    mbus_append_crc(pb);
    give_credits(msc, 15);
    return pb;
  }
  const uint8_t *hdr = pbuf_cdata(pb, 0);
  uint8_t flags = hdr[3];
  pb = pbuf_drop(pb, 4);
  if(flags & SP_EOS) {
    evlog(LOG_DEBUG, "%s: SP_EOS", msc->msc_name);
    net_task_raise(&msc->msc_task, SERVICE_EVENT_CLOSE);
    pbuf_free(pb);
  } else {
    msc->msc_remote_xmit_credits--;
    pb->pb_flags = (pb->pb_flags & ~3) | (flags & 3);
    mbus_seqpkt_txq_enq(msc, pb);
    net_task_raise(&msc->msc_task, SERVICE_EVENT_WAKEUP);
  }
  return NULL;
}

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
  mni->mni_tx_packets++;

  mbus_svc_t *ms = (mbus_svc_t *)mni;

  if(pbuf_pullup(pb, 4)) {
    panic("pullup failed");
  }
  const uint8_t *pkt = pbuf_cdata(pb, 0);
  const uint8_t dst_addr = pkt[0] & 0x3f;
  if(dst_addr < 32) {
    // Unicast
    const uint16_t flow = pkt[2] | ((pkt[1] << 3) & 0x300);
    const uint8_t src_addr = pkt[1] & 0x1f;

    const uint32_t tag = flow | (src_addr << 16) | (dst_addr << 24);
    mbus_seqpkt_con_t *msc = flow_find(ms, tag, 0);
    if(msc != NULL) {
      pb = pbuf_drop(pb, 3); // Drop header
      pbuf_trim(pb, 4); // Trim CRC
      return msc->msc_flow.mf_input(&msc->msc_flow, pb);
    }
  }
  STAILQ_INSERT_TAIL(&ms->ms_tx_queue, pb, pb_link);
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

  pb = ms_inspect_from_svc(ms, pb);
  if(pb == NULL)
    return NULL;

  STAILQ_INSERT_TAIL(&ms->ms_mni.mni_ni.ni_rx_queue, pb, pb_link);
  netif_wakeup(&ms->ms_mni.mni_ni);
  return NULL;
}


static void
ms_close(void *opaque)
{
  mbus_svc_t *ms = opaque;
  ms_remove_all_flows(ms);
  ms->ms_cb(ms->ms_opaque, SERVICE_EVENT_CLOSE);
  mbus_netif_detach(&ms->ms_mni); // also does free(ms)
}


SERVICE_DEF("mbus", 3, SERVICE_TYPE_DGRAM,
            ms_open, ms_push, ms_may_push, ms_pull, ms_close);
