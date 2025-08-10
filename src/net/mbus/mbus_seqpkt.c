#include "mbus_seqpkt.h"

#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/service.h>
#include <mios/eventlog.h>

#include "net/pbuf.h"
#include "mbus.h"

#include "irq.h"
#include "mbus_seqpkt_defs.h"

static void mbus_seqpkt_rtx_timer(void *opaque, uint64_t expire);

static void mbus_seqpkt_ack_timer(void *opaque, uint64_t expire);

static void mbus_seqpkt_ka_timer(void *opaque, uint64_t expire);

static void mbus_seqpkt_task_cb(net_task_t *nt, uint32_t signals);

static void mbus_seqpkt_maybe_destroy(mbus_seqpkt_con_t *msc);

static pbuf_t *mbus_seqpkt_input(mbus_flow_t *mf, pbuf_t *pb);


static uint32_t
mbus_seqpkt_local_flow_get_header(void *opaque)
{
  mbus_seqpkt_con_t *msc = opaque;
  extern uint8_t mbus_local_addr;

  return
    msc->msc_flow.mf_remote_addr |
    ((mbus_local_addr | ((msc->msc_flow.mf_flow >> 3) & 0x60)) << 8) |
    (msc->msc_flow.mf_flow << 16) |
    (msc->msc_local_flags << 24);
}


static pbuf_t *
mbus_seqpkt_local_flow_xmit(pbuf_t *pb, mbus_seqpkt_con_t *msc)
{
  return mbus_output_flow(pb, &msc->msc_flow);
}


static void
mbus_seqpkt_service_event_cb(void *opaque, uint32_t events)
{
  mbus_seqpkt_con_t *msc = opaque;
  net_task_raise(&msc->msc_task, events);
}

void
mbus_seqpkt_txq_enq(mbus_seqpkt_con_t *msc, struct pbuf *pb)
{
  pbuf_t *n;
  for(; pb != NULL; pb = n) {
    n = pb->pb_next;

    if(msc->msc_seqgen & 1)
      pb->pb_flags |= PBUF_SEQ;
    else
      pb->pb_flags &= ~PBUF_SEQ;

    msc->msc_seqgen++;
    msc->msc_txq_len++;
    STAILQ_INSERT_TAIL(&msc->msc_txq, pb, pb_link);
  }
}

static int
mbus_seqpkt_service_prep_send(mbus_seqpkt_con_t *msc)
{
  if(msc->msc_flow.mf_input == NULL)
    return 1;

  if(msc->msc_app_closed)
    return 0;

  if(msc->msc_sock.app_opaque == NULL)
    return 0;

  const pushpull_app_fn_t *app = msc->msc_sock.app;

  if(app->pull == NULL)
    return 0;

  while(msc->msc_txq_len < 2) {
    pbuf_t *p = app->pull(msc->msc_sock.app_opaque);
    if(p == NULL)
      break;
    mbus_seqpkt_txq_enq(msc, p);
  }
  return 0;
}

static uint8_t
mbus_seqpkt_service_update_cts(mbus_seqpkt_con_t *msc)
{
  uint8_t prev = msc->msc_local_flags;
  const pushpull_app_fn_t *app = msc->msc_sock.app;

  if(msc->msc_sock.app_opaque != NULL && app->may_push &&
     app->may_push(msc->msc_sock.app_opaque)) {
    msc->msc_local_flags |= SP_CTS;
  } else {
    msc->msc_local_flags &= ~SP_CTS;
  }
  return (msc->msc_local_flags ^ prev) ? SP_XMIT_CTS_CHANGED : 0;
}


static pbuf_t *
mbus_seqpkt_service_recv(pbuf_t *pb, mbus_seqpkt_con_t *msc)
{
  const pushpull_app_fn_t *app = msc->msc_sock.app;

  if(msc->msc_sock.app_opaque != NULL && app->push != NULL) {

    STAILQ_INSERT_TAIL(&msc->msc_rxq, pb, pb_link);

    if(pb->pb_flags & PBUF_EOP) {
      pb = pbuf_splice(&msc->msc_rxq);
      uint32_t events = app->push(msc->msc_sock.app_opaque, pb);
      if(events)
        net_task_raise(&msc->msc_task, events);

    }
    pb = NULL;
  }
  return pb;
}

static void
mbus_seqpkt_service_shut(mbus_seqpkt_con_t *msc, const char *reason)
{
  if(msc->msc_sock.app_opaque) {

    const pushpull_app_fn_t *app = msc->msc_sock.app;
    app->close(msc->msc_sock.app_opaque, reason);
    msc->msc_sock.app_opaque = NULL;
  }
}




static void
mbus_seqpkt_accept_err(const char *name, const char *reason,
                       uint8_t remote_addr)
{
  evlog(LOG_WARNING, "seqpkt/%s: Remote %d unable to connect -- %s",
        name, remote_addr, reason);
}


void
mbus_seqpkt_con_init(mbus_seqpkt_con_t *msc)
{
  msc->msc_task.nt_cb = mbus_seqpkt_task_cb;
  msc->msc_new_fragment = 1;

  STAILQ_INIT(&msc->msc_rxq);
  STAILQ_INIT(&msc->msc_txq);

  msc->msc_rtx_timer.t_cb = mbus_seqpkt_rtx_timer;
  msc->msc_rtx_timer.t_opaque = msc;
  msc->msc_rtx_timer.t_name = "seqpkt";

  msc->msc_ack_timer.t_cb = mbus_seqpkt_ack_timer;
  msc->msc_ack_timer.t_opaque = msc;
  msc->msc_ack_timer.t_name = "seqpkt";

  msc->msc_ka_timer.t_cb = mbus_seqpkt_ka_timer;
  msc->msc_ka_timer.t_opaque = msc;
  msc->msc_ka_timer.t_name = "seqpkt";

  msc->msc_last_rx = clock_get();

  msc->msc_flow.mf_input = mbus_seqpkt_input;

  net_timer_arm(&msc->msc_ka_timer, msc->msc_last_rx + SP_TIME_FAST_ACK);
}

static const pushpull_net_fn_t mbus_seqpkt_fn = {
  .event = mbus_seqpkt_service_event_cb,
  .get_flow_header = mbus_seqpkt_local_flow_get_header,
};


pbuf_t *
mbus_seqpkt_accept(pbuf_t *pb, uint8_t remote_addr, uint16_t flow)
{
  uint8_t *pkt = pbuf_data(pb, 0);
  size_t len = pb->pb_pktlen;

  pkt[len] = 0; // Zero-terminate service name (safe, CRC was here before)
  const char *name = (const char *)pkt;

  evlog(LOG_INFO, "seqpkt/%s: Connect from addr %d", name, remote_addr);

  const service_t *s = service_find_by_name(name);
  if(s == NULL) {
    mbus_seqpkt_accept_err(name, "Not available", remote_addr);
    return pb;
  }

  mbus_seqpkt_con_t *msc = xalloc(sizeof(mbus_seqpkt_con_t), 0, MEM_MAY_FAIL);
  if(msc == NULL) {
    mbus_seqpkt_accept_err(name, "No memory", remote_addr);
    return pb;
  }
  memset(msc, 0, sizeof(mbus_seqpkt_con_t));

  msc->msc_sock.max_fragment_size = MBUS_FRAGMENT_SIZE;
  msc->msc_sock.net = &mbus_seqpkt_fn;
  msc->msc_sock.net_opaque = msc;

  error_t err = s->open_pushpull(&msc->msc_sock);
  if(err) {
    free(msc);
    mbus_seqpkt_accept_err(name, error_to_string(err), remote_addr);
    return pb;
  }

  msc->msc_name = s->name;

  // Network interface
  msc->msc_xmit = mbus_seqpkt_local_flow_xmit;

  // Service/App interface
  msc->msc_update_local_cts = mbus_seqpkt_service_update_cts;
  msc->msc_prep_send = mbus_seqpkt_service_prep_send;
  msc->msc_recv = mbus_seqpkt_service_recv;
  msc->msc_shut_app = mbus_seqpkt_service_shut;

  mbus_seqpkt_con_init(msc);

  msc->msc_flow.mf_flow = flow;
  msc->msc_flow.mf_remote_addr = remote_addr;

  mbus_flow_insert(&msc->msc_flow);

  pbuf_trim(pb, len - 1);
  pkt[0] = msc->msc_local_flags;
  msc->msc_local_flags_sent = msc->msc_local_flags | SP_SEQ;

  return mbus_output_flow(pb, &msc->msc_flow);
}





static pbuf_t *
pbuf_for_xmit(mbus_seqpkt_con_t *msc, pbuf_t *pb)
{
  if(pb == NULL) {
    pb = pbuf_make(3, 0);
    if(pb == NULL) {
      // No buffers available, retry soon
      net_timer_arm(&msc->msc_rtx_timer, clock_get() + SP_TIME_RTX);
      return NULL;
    }
  } else {
    pbuf_reset(pb, 3, 0);
  }
  return pb;
}




static pbuf_t *
mbus_seqpkt_output(mbus_seqpkt_con_t *msc, pbuf_t *pb, uint32_t xmit)
{
  if(msc->msc_prep_send && msc->msc_prep_send(msc))
    return pb;

  pbuf_t *tx = STAILQ_FIRST(&msc->msc_txq);
  if(tx != NULL) {
    const int send_seq = !!(tx->pb_flags & SP_SEQ);
    const int expected_seq = !!(msc->msc_remote_flags & SP_ESEQ);

    if(send_seq != expected_seq || !(msc->msc_remote_flags & SP_CTS)) {
      tx = NULL;
    } else if(msc->msc_new_fragment) {
      msc->msc_new_fragment = 0;
      xmit |= SP_XMIT_NEW_FRAGMENT;
    }
  }

  xmit |= msc->msc_app_closed ? SP_XMIT_CLOSE : 0;

  xmit |= msc->msc_update_local_cts(msc);

  if(!xmit)
    return pb;

  pb = pbuf_for_xmit(msc, pb);
  if(pb == NULL)
    return NULL;

  msc->msc_last_tx = clock_get();

  uint8_t *hdr = pbuf_append(pb, 1);
  hdr[0] = msc->msc_local_flags;

  if(tx != NULL && (xmit & (SP_XMIT_NEW_FRAGMENT | SP_XMIT_RTX))) {

    msc->msc_local_flags =
      (msc->msc_local_flags & ~SP_SEQ) | (tx->pb_flags & SP_SEQ);

    hdr[0] = msc->msc_local_flags | (tx->pb_flags & (SP_FF | SP_LF));

    if(STAILQ_NEXT(tx, pb_link))
      hdr[0] |= SP_MORE;

    uint8_t *payload = pbuf_append(pb, tx->pb_buflen);
    memcpy(payload, pbuf_cdata(tx, 0), tx->pb_buflen);

    msc->msc_rtx_attempt++;
    net_timer_arm(&msc->msc_rtx_timer,
                  msc->msc_last_tx + SP_TIME_RTX * msc->msc_rtx_attempt);

  } else if(msc->msc_app_closed) {

    timer_disarm(&msc->msc_ack_timer);
    hdr[0] |= SP_EOS; // Send close
    msc->msc_local_flags_sent = hdr[0];
    pb = msc->msc_xmit(pb, msc);
    msc->msc_shut_app(msc, "Close sent");
    return pb;
  }

  timer_disarm(&msc->msc_ack_timer);
  msc->msc_local_flags_sent = hdr[0];
  return msc->msc_xmit(pb, msc);
}


static void
release_txq(mbus_seqpkt_con_t *msc)
{
  pbuf_t *pb = STAILQ_FIRST(&msc->msc_txq);
  if(pb == NULL)
    return;

  const int sent_seq = !!(pb->pb_flags & SP_SEQ);
  const int expected_seq = !!(msc->msc_remote_flags & SP_ESEQ);
  if(sent_seq == expected_seq)
    return;

  STAILQ_REMOVE_HEAD(&msc->msc_txq, pb_link);
  msc->msc_txq_len--;
  pb->pb_next = NULL;
  pbuf_free(pb);
  timer_disarm(&msc->msc_rtx_timer);
  msc->msc_rtx_attempt = 0;
  msc->msc_new_fragment = 1;
  msc->msc_remote_avail_credits++;

  if(msc->msc_post_send)
    msc->msc_post_send(msc);
}


static pbuf_t *
mbus_seqpkt_recv_data(mbus_seqpkt_con_t *msc, pbuf_t *pb)
{
  uint8_t *pkt = pbuf_data(pb, 0);

  msc->msc_last_rx = clock_get();
  msc->msc_remote_flags = pkt[0];

  release_txq(msc);

  pb = pbuf_drop(pb, 1, 0);

  if(pb->pb_pktlen) {

    const int recv_seq = !!(msc->msc_remote_flags & SP_SEQ);
    const int expect_seq = !!(msc->msc_local_flags & SP_ESEQ);

    if(recv_seq == expect_seq && msc->msc_local_flags & SP_CTS) {

      msc->msc_local_flags ^= SP_ESEQ;

      const int ack_time = msc->msc_remote_flags & SP_MORE ?
        SP_TIME_FAST_ACK : SP_TIME_ACK;
      net_timer_arm(&msc->msc_ack_timer, msc->msc_last_rx + ack_time);

      pb->pb_flags = msc->msc_remote_flags & (SP_FF | SP_LF);

      pb = msc->msc_recv(pb, msc);
    }
  }

  pb = mbus_seqpkt_output(msc, pb, 0);
  mbus_seqpkt_maybe_destroy(msc);
  return pb;
}


static pbuf_t *
mbus_seqpkt_input(mbus_flow_t *mf, pbuf_t *pb)
{
  mbus_seqpkt_con_t *msc = (mbus_seqpkt_con_t *)mf;
  const uint8_t *pkt = pbuf_data(pb, 0);
  if(pb->pb_pktlen < 1)
    return pb;

  if(pkt[0] & SP_EOS) {
    evlog(LOG_DEBUG, "seqpkt/%s: Close received", msc->msc_name);
    msc->msc_shut_app(msc, "Peer closed");
    mbus_flow_remove(&msc->msc_flow);
    mbus_seqpkt_maybe_destroy(msc);
    return pb;
  }
  return mbus_seqpkt_recv_data(msc, pb);
}


static void
mbus_seqpkt_maybe_destroy(mbus_seqpkt_con_t *msc)
{
  if(!msc->msc_app_closed || msc->msc_sock.app_opaque)
    return;

  pbuf_free(STAILQ_FIRST(&msc->msc_rxq));
  pbuf_free(STAILQ_FIRST(&msc->msc_txq));
  timer_disarm(&msc->msc_rtx_timer);
  timer_disarm(&msc->msc_ack_timer);
  timer_disarm(&msc->msc_ka_timer);
  mbus_flow_remove(&msc->msc_flow);
  evlog(LOG_DEBUG, "seqpkt/%s: Destroyed", msc->msc_name);
  free(msc);
}


static void
mbus_seqpkt_tick(mbus_seqpkt_con_t *msc, uint32_t xmit)
{
  pbuf_t *pb = mbus_seqpkt_output(msc, NULL, xmit);
  pbuf_free(pb);
  mbus_seqpkt_maybe_destroy(msc);
}

static void
mbus_seqpkt_task_cb(net_task_t *nt, uint32_t signals)
{
  mbus_seqpkt_con_t *msc =
    ((void *)nt) - offsetof(mbus_seqpkt_con_t, msc_task);

  if(signals & PUSHPULL_EVENT_CLOSE) {
    evlog(LOG_DEBUG, "seqpkt/%s: App side closed", msc->msc_name);
    msc->msc_app_closed = 1;
  }

  mbus_seqpkt_tick(msc, 0);
}


static void
mbus_seqpkt_rtx_timer(void *opaque, uint64_t now)
{
  mbus_seqpkt_tick(opaque, SP_XMIT_RTX);
}


static void
mbus_seqpkt_ack_timer(void *opaque, uint64_t now)
{
  mbus_seqpkt_tick(opaque, SP_XMIT_ESEQ_CHANGED);
}


static void
mbus_seqpkt_ka_timer(void *opaque, uint64_t now)
{
  mbus_seqpkt_con_t *msc = opaque;

  if(now > msc->msc_last_rx + SP_TIME_TIMEOUT) {
    msc->msc_shut_app(msc, "Timeout");
  } else {
    net_timer_arm(&msc->msc_ka_timer, now + SP_TIME_KA);
    if(now > msc->msc_last_tx + SP_TIME_KA)
      mbus_seqpkt_tick(msc, SP_XMIT_KA);
  }
}
