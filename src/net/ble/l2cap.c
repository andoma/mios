#include "l2cap.h"

#include "irq.h"

#include "ble_proto.h"
#include "l2cap_proto.h"

#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <sys/param.h>

#include <mios/service.h>
#include <mios/pushpull.h>
#include <mios/eventlog.h>

/*
 *  Based on core_v5.3  Vol 3(Host) Part A
 */

#define L2CAP_SIGNAL_INPUT      0x1
#define L2CAP_SIGNAL_DISCONNECT 0x2

typedef struct l2cap_connection {

  net_task_t lc_task;

  LIST_ENTRY(l2cap_connection) lc_link;

  l2cap_t *lc_l2c;

  pushpull_t lc_pushpull;

  int lc_remote_credits;
  int lc_local_credits;

  struct pbuf_queue lc_rxq;
  uint16_t lc_credit_deficit;
  uint16_t lc_credit_threshold;

  struct pbuf_queue lc_rfq;

  uint16_t lc_remote_mtu;
  uint16_t lc_remote_mps;
  uint16_t lc_psm;

  uint8_t lc_local_cid;
  uint8_t lc_remote_cid;

  uint8_t lc_reframing;

  int lc_refcount;

  int lc_queued_credits;

  const char *lc_name;

} l2cap_connection_t;

static void
connection_release(l2cap_connection_t *lc)
{
  if(__sync_add_and_fetch(&lc->lc_refcount, -1))
    return;
  free(lc);
}



__attribute__((warn_unused_result))
pbuf_t *
l2cap_splice(struct pbuf_queue *pq, int header_size)
{
  pbuf_t *pb = STAILQ_FIRST(pq);
  if(pb == NULL)
    return NULL;

  if(pbuf_pullup(pb, header_size))
    return NULL;

  pbuf_t *last = pb;
  last->pb_flags |= PBUF_SOP;

  const uint16_t *hdr = pbuf_cdata(pb, 0);
  const int expected_length = hdr[0] + header_size;
  if(expected_length > 200) {
    pbuf_dump("badlen", pb, 1);
    panic("High unexpected length %d",expected_length);
  }
  int sum = 0;
  int count = 1;
  while(1) {
    if(sum + last->pb_buflen > expected_length) {
      size_t splitsize = sum + last->pb_buflen - expected_length;
      pbuf_t *s = pbuf_make(0, 0);
      if(s == NULL)
        return NULL;
      STAILQ_INSERT_AFTER(pq, last, s, pb_link);
      last->pb_buflen -= splitsize;
      memcpy(s->pb_data, last->pb_data + last->pb_offset + last->pb_buflen,
             splitsize);
      s->pb_buflen = splitsize;

      s->pb_credits += last->pb_credits;
      last->pb_credits = 0;
    }
    sum += last->pb_buflen;
    if(sum == expected_length)
      break;
    last->pb_flags &= ~PBUF_EOP;
    last = STAILQ_NEXT(last, pb_link);
    if(last == NULL)
      break;
    count++;
    last->pb_flags &= ~PBUF_SOP;
  }

  if(last == NULL)
    return NULL;
  pb->pb_pktlen = expected_length;
  STAILQ_REMOVE_HEAD_UNTIL(pq, last, pb_link);
  last->pb_next = NULL;
  last->pb_flags |= PBUF_EOP;
  return pb;
}


static void
l2cap_output(l2cap_t *l2c, pbuf_t *pb, uint16_t cid)
{
  const uint16_t len = pb->pb_pktlen;

  pb = pbuf_prepend(pb, sizeof(l2cap_header_t), 1, 2);
  l2cap_header_t *hdr = pbuf_data(pb, 0);
  hdr->pdu_length = len;
  hdr->channel_id = cid;
  l2c->l2c_output(l2c, pb);
}


static void
con_output(l2cap_connection_t *lc, pbuf_t *pb)
{
  if(lc->lc_reframing) {
    pb = pbuf_prepend(pb, 2, 1, sizeof(l2cap_header_t) + 2 + 2);
    uint16_t *reframelen = pbuf_data(pb, 0);
    *reframelen = pb->pb_pktlen - 2;
  }

  lc->lc_remote_credits--;

  assert(pb->pb_pktlen < lc->lc_remote_mtu);

  pb = pbuf_prepend(pb, 2, 1, sizeof(l2cap_header_t) + 2);
  uint16_t *sdulen = pbuf_data(pb, 0);
  *sdulen = pb->pb_pktlen - 2;

  if(pb->pb_pktlen <= lc->lc_remote_mps)
    return l2cap_output(lc->lc_l2c, pb, lc->lc_remote_cid);

  pbuf_t *o = pbuf_make(2, 1);
  o->pb_buflen = sizeof(l2cap_header_t);

  l2cap_header_t *hdr = pbuf_data(o, 0);
  hdr->pdu_length = pb->pb_pktlen;
  hdr->channel_id = lc->lc_remote_cid;

  l2cap_t *l2c = lc->lc_l2c;

  while(1) {
    size_t frag = MIN(LLMTU, lc->lc_remote_mps) - o->pb_buflen;
    frag = MIN(frag, pb->pb_pktlen);
    pb = pbuf_read(pb, o->pb_data + o->pb_offset + o->pb_buflen, frag);

    assert(o->pb_offset + frag <= LLMTU);
    o->pb_buflen += frag;
    o->pb_pktlen = o->pb_buflen;

    l2c->l2c_output(l2c, o);
    if(pb == NULL)
      break;

    o = pbuf_make(2, 1);
    if(o == NULL)
      break;
    o->pb_buflen = 0;
    o->pb_flags = PBUF_EOP;
  }
}


static void
connection_pull(l2cap_connection_t *lc)
{
  while(lc->lc_remote_credits > 0 && lc->lc_pushpull.app_opaque) {
    pbuf_t *pb = lc->lc_pushpull.app->pull(lc->lc_pushpull.app_opaque);
    if(pb == NULL)
      break;
    con_output(lc, pb);
  }
}


static void
connection_close(l2cap_connection_t *lc, const char *why)
{
  evlog(LOG_DEBUG, "l2cap: Connection closed -- %s", why);

  lc->lc_l2c = NULL;
  LIST_REMOVE(lc, lc_link);
  lc->lc_pushpull.app->close(lc->lc_pushpull.app_opaque, why);
  lc->lc_pushpull.app_opaque = NULL;
}


static void
connection_clear_credit_deficit(l2cap_connection_t *lc)
{
  if(lc->lc_credit_deficit < lc->lc_credit_threshold)
    return;

  l2cap_t *l2c = lc->lc_l2c;
  if(l2c == NULL)
    return;

  pbuf_t *pb = pbuf_make(8, 0);
  if(pb == NULL)
    return;

  l2cap_flow_control_credit_ind_t *ind =
    pbuf_append(pb, sizeof(l2cap_flow_control_credit_ind_t));

  ind->cid = lc->lc_local_cid;
  ind->credits = lc->lc_credit_deficit;
  lc->lc_credit_deficit = 0;
  ind->hdr.identifier = 1;
  ind->hdr.data_length = 4;
  ind->hdr.code = L2CAP_FLOW_CONTROL_CREDIT_IND;
  l2cap_output(l2c, pb, L2CAP_CID_LE_SIGNALING);
}


static int
count_credits(const pbuf_t *pb)
{
  int c = 0;
  for(; pb != NULL; pb = pb->pb_next) {
    c += pb->pb_credits;
  }
  return c;
}

static pbuf_t *
connection_push(l2cap_connection_t *lc)
{
  pbuf_t *pb = NULL;
  while(1) {
    if(!lc->lc_pushpull.app->may_push(lc->lc_pushpull.app_opaque))
      return pb;

    pbuf_t *pb2 = l2cap_splice(&lc->lc_rxq, 2);
    if(pb2 == NULL)
      return pb;
    if(pb)
      pbuf_free(pb);
    pb = pbuf_drop(pb2, 2);
    if(pbuf_pullup(pb, pb->pb_pktlen)) {
      panic("pullup failed");
    }

    if(lc->lc_reframing) {
      STAILQ_INSERT_TAIL(&lc->lc_rfq, pb, pb_link);
      pb = NULL;
      while(1) {

        if(!lc->lc_pushpull.app->may_push(lc->lc_pushpull.app_opaque)) {
          break;
        }

        pb2 = l2cap_splice(&lc->lc_rfq, 2);
        if(pb2 == NULL)
          break;

        if(pb)
          pbuf_free(pb);
        pb = pbuf_drop(pb2, 2);
        if(pbuf_pullup(pb, pb->pb_pktlen)) {
          panic("pullup failed");
        }

        int credits = count_credits(pb);
        lc->lc_credit_deficit += credits;
        lc->lc_queued_credits -= credits;
        assert(lc->lc_queued_credits >= 0);
        if(lc->lc_pushpull.app->push(lc->lc_pushpull.app_opaque, pb)) {

        }
        pb = NULL;
      }
    } else {
      int credits = count_credits(pb);
      lc->lc_credit_deficit += credits;

      if(lc->lc_pushpull.app->push(lc->lc_pushpull.app_opaque, pb)) {

      }
      pb = NULL;
    }
  }
}


static void
connection_task_cb(net_task_t *task, uint32_t signals)
{
  l2cap_connection_t *lc = (l2cap_connection_t *)task;

  l2cap_t *l2c = lc->lc_l2c;

  if(signals & PUSHPULL_EVENT_CLOSE) {

    if(l2c) {
      evlog(LOG_DEBUG, "l2cap: Connection closed by service end");

      pbuf_t *pb = pbuf_make(8, 0);
      if(pb != NULL) {

        l2cap_disconnection_req_t *req =
          pbuf_append(pb, sizeof(l2cap_disconnection_req_t));

        req->dst_cid = lc->lc_remote_cid;
        req->src_cid = lc->lc_local_cid;
        req->hdr.identifier = 1;
        req->hdr.data_length = 4;
        req->hdr.code = L2CAP_DISCONNECTION_REQ;
        l2cap_output(l2c, pb, L2CAP_CID_LE_SIGNALING);
      }
      LIST_REMOVE(lc, lc_link);
    }
    int q = irq_forbid(IRQ_LEVEL_NET);
    pbuf_free_queue_irq_blocked(&lc->lc_rxq);
    pbuf_free_queue_irq_blocked(&lc->lc_rfq);
    irq_permit(q);
    connection_release(lc);
    return;
  }

  if(signals & PUSHPULL_EVENT_PULL && l2c) {
    connection_pull(lc);
  }
}


__attribute__((warn_unused_result))
static l2cap_connection_t *
connection_create(l2cap_t *l2c)
{
  int cid;

  for(cid = 0x40; cid < 0x80; cid++) {
    l2cap_connection_t *lc;
    LIST_FOREACH(lc, &l2c->l2c_connections, lc_link) {
      if(lc->lc_local_cid == cid)
        break;
    }
    if(lc)
      continue;

    lc = xalloc(sizeof(l2cap_connection_t), 0, MEM_MAY_FAIL);
    if(lc == NULL)
      return NULL;
    memset(lc, 0, sizeof(l2cap_connection_t));

    lc->lc_refcount = 1;
    STAILQ_INIT(&lc->lc_rxq);
    STAILQ_INIT(&lc->lc_rfq);

    lc->lc_task.nt_cb = connection_task_cb;
    lc->lc_local_cid = cid;
    lc->lc_l2c = l2c;
    return lc;
  }
  return NULL;
}


static void
l2c_service_event_cb(void *opaque, uint32_t events)
{
  l2cap_connection_t *lc = opaque;
  net_task_raise(&lc->lc_task, events);
}



static void
handle_le_credit_based_connection_fail(l2cap_t *l2c, pbuf_t *pb,
                                       const char *msg, int code)
{
  const l2cap_le_credit_based_connection_req_t *req = pbuf_data(pb, 0);

  evlog(LOG_ERR,
        "l2cap: Connection [SPSM:0x%x CID:0x%x MTU:%d MPS:%d IC:%d] "
        " -- Failed: %s",
        req->spsm, req->src_cid, req->mtu, req->mps, req->initial_credits,
        msg);

  l2cap_le_credit_based_connection_rsp_t *rsp = pbuf_data(pb, 0);

  rsp->result = code;

  return l2cap_output(l2c, pb, L2CAP_CID_LE_SIGNALING);
}



static const pushpull_net_fn_t l2cap_fns = {
  . event = &l2c_service_event_cb,
};


static void
handle_le_credit_based_connection_req(l2cap_t *l2c, pbuf_t *pb)
{
  const l2cap_le_credit_based_connection_req_t *req = pbuf_data(pb, 0);

  l2cap_le_credit_based_connection_rsp_t *rsp = pbuf_data(pb, 0);
  // We reuse the packet and send it back
  rsp->hdr.code = L2CAP_LE_CREDIT_BASED_CONNECTION_RSP;

  const service_t *s = NULL;
  if(req->spsm > 0x80 && req->spsm < 0x100) {
    s = service_find_by_ble_psm(req->spsm & 0x3f);
  }

  if(s == NULL) {
    return handle_le_credit_based_connection_fail(l2c, pb, "No service",
                                                  L2CAP_CON_NO_PSM);
  }

  l2cap_connection_t *lc = connection_create(l2c);
  if(lc == NULL) {
    return handle_le_credit_based_connection_fail(l2c, pb, "No memory",
                                                  L2CAP_CON_NO_RESOURCES);
  }

  size_t hdrs = 2 + sizeof(l2cap_header_t) + 2 + lc->lc_reframing * 2;

  lc->lc_reframing = !!(req->spsm & 0x40);
  lc->lc_remote_cid = req->src_cid;
  lc->lc_psm = req->spsm;
  lc->lc_remote_mtu = req->mtu;
  lc->lc_remote_mps = MIN(req->mps, PBUF_DATA_SIZE - hdrs);
  lc->lc_remote_credits = req->initial_credits;

  lc->lc_pushpull.max_fragment_size = lc->lc_remote_mps;
  lc->lc_pushpull.preferred_offset = hdrs;

  lc->lc_pushpull.net_opaque = lc;
  lc->lc_pushpull.net = &l2cap_fns;

  if(s->open_pushpull(&lc->lc_pushpull)) {
    free(lc);
    return handle_le_credit_based_connection_fail(l2c, pb, "Unable to setup",
                                                  L2CAP_CON_NO_RESOURCES);
  }

  lc->lc_name = s->name;

  evlog(LOG_INFO,
        "l2cap: Connect [SPSM:0x%x CID:0x%x MTU:%d MPS:%d IC:%d]"
        " -- OK: %s",
        req->spsm, req->src_cid, req->mtu, req->mps, req->initial_credits,
        s->name);


  LIST_INSERT_HEAD(&l2c->l2c_connections, lc, lc_link);

  rsp->dst_cid = lc->lc_local_cid;
  rsp->result = 0;
  rsp->mtu = LLMTU;
  rsp->mps = PBUF_DATA_SIZE;
  rsp->initial_credits = 65535;
  lc->lc_credit_threshold = 2;
  lc->lc_local_credits = rsp->initial_credits;

  return l2cap_output(l2c, pb, L2CAP_CID_LE_SIGNALING);
}



static void
handle_disconnection_req(l2cap_t *l2c, pbuf_t *pb)
{
  l2cap_disconnection_req_t *req = pbuf_data(pb, 0);

  uint16_t src_cid = req->src_cid;
  uint16_t dst_cid = req->dst_cid;

  l2cap_connection_t *lc;

  LIST_FOREACH(lc, &l2c->l2c_connections, lc_link) {
    if(dst_cid == lc->lc_local_cid && src_cid == lc->lc_remote_cid) {
      connection_close(lc, "Closed by peer");
      break;
    }
  }

  l2cap_disconnection_rsp_t *rsp = pbuf_data(pb, 0);
  rsp->src_cid = dst_cid;
  rsp->dst_cid = src_cid;
  // We reuse the packet and send it back
  rsp->hdr.code = L2CAP_DISCONNECTION_RSP;

  return l2cap_output(l2c, pb, L2CAP_CID_LE_SIGNALING);

}

static void
handle_flow_control_credit_ind(l2cap_t *l2c, pbuf_t *pb)
{
  const l2cap_flow_control_credit_ind_t *cc = pbuf_cdata(pb, 0);

  l2cap_connection_t *lc;
  LIST_FOREACH(lc, &l2c->l2c_connections, lc_link) {
    if(cc->cid == lc->lc_remote_cid) {
      break;
    }
  }
  if(lc == NULL)
    return;

  lc->lc_remote_credits += cc->credits;
  connection_pull(lc);
}

__attribute__((warn_unused_result))
static pbuf_t *
handle_le_signaling(l2cap_t *l2c, pbuf_t *pb)
{
  const l2cap_cframe_t *cf = pbuf_cdata(pb, 0);

  switch(cf->code) {
  case L2CAP_LE_CREDIT_BASED_CONNECTION_REQ:
    handle_le_credit_based_connection_req(l2c, pb);
    break;
  case L2CAP_DISCONNECTION_REQ:
    handle_disconnection_req(l2c, pb);
    break;
  case L2CAP_FLOW_CONTROL_CREDIT_IND:
    handle_flow_control_credit_ind(l2c, pb);
    return pb;
  case L2CAP_DISCONNECTION_RSP:
    return pb;
  default:
    evlog(LOG_DEBUG, "l2cap_le_signaling: Unsupported code: 0x%x", cf->code);
    return pb;
  }
  return NULL;
}



/***************************************************************************
 * ATT
 */
static void
att_output(l2cap_t *l2c, pbuf_t *pb, uint8_t opcode)
{
  pb = pbuf_prepend(pb, 1, 1, 0);
  uint8_t *hdr = pbuf_data(pb, 0);
  *hdr = opcode;
  return l2cap_output(l2c, pb, L2CAP_CID_ATT);
}


static void
handle_att_exchange_mtu_req(l2cap_t *l2c, pbuf_t *pb)
{
  l2cap_att_mtu_t *pkt = pbuf_data(pb, 0);
  pkt->mtu = 23;
  return att_output(l2c, pb, ATT_EXCHANGE_MTU_RSP);
}

static void
att_send_error(l2cap_t *l2c, pbuf_t *pb,
               uint8_t request_opcode, uint16_t handle, uint8_t error)
{
  pbuf_reset(pb, pb->pb_offset, sizeof(l2cap_att_error_t));

  l2cap_att_error_t *rsp = pbuf_data(pb, 0);
  rsp->request_opcode = request_opcode;
  rsp->attribute_handle = handle;
  rsp->error_code = error;
  return att_output(l2c, pb, ATT_ERROR_RSP);
}


static void
handle_att_read_by_type(l2cap_t *l2c, pbuf_t *pb)
{
  const l2cap_att_read_by_type_req_t *req = pbuf_data(pb, 0);
  return att_send_error(l2c, pb, ATT_READ_BY_TYPE_REQ,
                        req->starting_handle, 0xa);
}


static void
handle_att_read_by_group_type(l2cap_t *l2c, pbuf_t *pb)
{
  const l2cap_att_read_by_type_req_t *req = pbuf_data(pb, 0);
  return att_send_error(l2c, pb, ATT_READ_BY_GROUP_TYPE_REQ,
                        req->starting_handle, 0xa);
}

static void
handle_att_read(l2cap_t *l2c, pbuf_t *pb)
{
  const l2cap_att_read_t *req = pbuf_data(pb, 0);
  return att_send_error(l2c, pb, ATT_READ_BY_GROUP_TYPE_REQ,
                        req->handle, 1);
}


static void
handle_att(l2cap_t *l2c, pbuf_t *pb)
{
  const uint8_t *hdr = pbuf_cdata(pb, 0);

  const uint8_t opcode = *hdr;
  pb = pbuf_drop(pb, 1);

  switch(opcode) {
  case ATT_EXCHANGE_MTU_REQ:
    return handle_att_exchange_mtu_req(l2c, pb);
  case ATT_READ_BY_TYPE_REQ:
    return handle_att_read_by_type(l2c, pb);
  case ATT_READ_BY_GROUP_TYPE_REQ:
    return handle_att_read_by_group_type(l2c, pb);
  case ATT_READ_REQ:
    return handle_att_read(l2c, pb);
  default:
    evlog(LOG_DEBUG, "l2cap_att: Unsupporetd opcode: 0x%x", opcode);
    pbuf_free(pb);
    return;
  }
}




__attribute__((warn_unused_result))
static pbuf_t *
handle_channel(l2cap_t *l2c, pbuf_t *pb, uint16_t channel_id)
{
  l2cap_connection_t *lc;
  LIST_FOREACH(lc, &l2c->l2c_connections, lc_link) {
    if(lc->lc_local_cid == channel_id) {
      break;
    }
  }

  if(lc == NULL || lc->lc_pushpull.app_opaque == NULL) {
    evlog(LOG_DEBUG, "l2cap: Input on unexpected channel 0x%x", channel_id);
    return pb;
  }

  pbuf_t *next;
  for(; pb; pb = next) {
    pb->pb_credits = 0;
    next = pb->pb_next;
    if(!next) {
      pb->pb_credits = 1;
    }
    STAILQ_INSERT_TAIL(&lc->lc_rxq, pb, pb_link);
  }

  lc->lc_queued_credits++;
  pb = connection_push(lc);
  connection_clear_credit_deficit(lc);
  connection_pull(lc);
  return pb;
}


__attribute__((warn_unused_result))
static pbuf_t *
handle_packet(l2cap_t *l2c, pbuf_t *pb)
{
  if(pb->pb_buflen < 4)
    return pb;

  const l2cap_header_t *hdr = pbuf_cdata(pb, 0);

  int pdu_length = hdr->pdu_length;
  const int channel_id = hdr->channel_id;
  pb = pbuf_drop(pb, 4);

  if(pdu_length != pb->pb_pktlen) {
    printf("Got packet with unexpected pdu_length:%d pktlen:%d\n",
           pdu_length, pb->pb_pktlen);
    return pb;
  }

  switch(channel_id) {

  case L2CAP_CID_LE_SIGNALING:
    return handle_le_signaling(l2c, pb);

  case L2CAP_CID_ATT:
    handle_att(l2c, pb);
    return NULL;

  default:
    return handle_channel(l2c, pb, channel_id);
  }
}


// IRQ level
void
l2cap_input(l2cap_t *l2c, pbuf_t *pb)
{
  STAILQ_INSERT_TAIL(&l2c->l2c_rx_queue, pb, pb_link);
  assert(pb->pb_buflen > 0);
  net_task_raise(&l2c->l2c_task, L2CAP_SIGNAL_INPUT);
}


void
l2cap_disconnect(l2cap_t *l2c)
{
  net_task_raise(&l2c->l2c_task, L2CAP_SIGNAL_DISCONNECT);
}


static void
l2cap_dispatch_signal(struct net_task *nt, uint32_t signals)
{
  l2cap_t *l2c = (l2cap_t *)nt;
  if(signals & L2CAP_SIGNAL_DISCONNECT) {

    l2cap_connection_t *lc;
    while((lc = LIST_FIRST(&l2c->l2c_connections)) != NULL) {
      connection_close(lc, "Transport disconnect");
    }

    int q = irq_forbid(IRQ_LEVEL_NET);
    l2c->l2c_output(l2c, NULL);
    pbuf_free_queue_irq_blocked(&l2c->l2c_rx_queue);
    l2c->l2c_rx_queue_len = 0;
    irq_permit(q);
    return;
  }

  if(signals & L2CAP_SIGNAL_INPUT) {

    int q = irq_forbid(IRQ_LEVEL_NET);

    while(1) {
      pbuf_t *pb = STAILQ_FIRST(&l2c->l2c_rx_queue);
      if(pb == NULL)
        break;
      assert(pb->pb_flags == 1);
      assert(pb->pb_offset == 2);

      pb = l2cap_splice(&l2c->l2c_rx_queue, 4);
      if(pb == NULL)
        break;
      irq_permit(q);
      pb = handle_packet(l2c, pb);
      q = irq_forbid(IRQ_LEVEL_NET);
      if(pb)
        pbuf_free_irq_blocked(pb);
    }
    irq_permit(q);
  }
}

error_t
l2cap_connect(l2cap_t *l2cap)
{
  l2cap->l2c_task.nt_cb = l2cap_dispatch_signal;
  STAILQ_INIT(&l2cap->l2c_rx_queue);
  return 0;
}


static l2cap_connection_t *
l2cap_connection_next(l2cap_t *l2c, l2cap_connection_t *cur)
{
  int q = irq_forbid(IRQ_LEVEL_SWITCH);
  l2cap_connection_t *lc;

  if(cur == NULL) {
    lc = LIST_FIRST(&l2c->l2c_connections);
  } else {
    LIST_FOREACH(lc, &l2c->l2c_connections, lc_link) {
      if(lc == cur)
        break;
    }
    if(lc)
      lc = LIST_NEXT(lc, lc_link);
  }

  if(lc)
    __sync_add_and_fetch(&lc->lc_refcount, 1);

  irq_permit(q);

  if(cur)
    connection_release(cur);
  return lc;
}

void
l2cap_print(l2cap_t *l2c, stream_t *st)
{
  l2cap_connection_t *lc = NULL;
  while((lc = l2cap_connection_next(l2c, lc)) != NULL) {
    stprintf(st, "\tL2CAP:\t%s (PSM:0x%x)\n", lc->lc_name, lc->lc_psm);

    stprintf(st, "\t\tLocal:  CID:%d Credits:%d\n",
             lc->lc_local_cid,
             lc->lc_local_credits);

    stprintf(st, "\t\tRemote: CID:%d MTU:%d MPS:%d Credits:%d\n",
             lc->lc_remote_cid,
             lc->lc_remote_mtu,
             lc->lc_remote_mps,
             lc->lc_remote_credits);
    stprintf(st, "\t\tRX credits: deficit:%d queued:%d\n",
             lc->lc_credit_deficit,
             lc->lc_queued_credits);
    stprintf(st, "\n");

    pbuf_dump_stream("RXQ", STAILQ_FIRST(&lc->lc_rxq), 0, st);
    pbuf_dump_stream("RFQ", STAILQ_FIRST(&lc->lc_rfq), 0, st);
  }
}

