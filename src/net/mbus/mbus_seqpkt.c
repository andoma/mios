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

#define MBUS_FRAGMENT_SIZE 56

#define SP_TIME_TIMEOUT  2500000
#define SP_TIME_KA       300000
#define SP_TIME_RTX      25000
#define SP_TIME_ACK      10000
#define SP_TIME_FAST_ACK 1000

#define SP_FF   0x1
#define SP_LF   0x2
#define SP_ESEQ 0x4
#define SP_SEQ  0x8
#define SP_CTS  0x10
#define SP_MORE 0x20

// We rely on these flags having the same value, so make sure that holds
_Static_assert(SP_FF  == PBUF_SOP);
_Static_assert(SP_LF  == PBUF_EOP);
_Static_assert(SP_SEQ == PBUF_SEQ);

LIST_HEAD(mbus_seqpkt_con_list, mbus_seqpkt_con);

// Transmit because sequence bumped, we're sending a new fragment
#define SP_XMIT_NEW_FRAGMENT  0x1

// Transmit because RTX timer has expired
#define SP_XMIT_RTX           0x2

// Transmit because CTS changed
#define SP_XMIT_CTS_CHANGED   0x4

// Transmit because expected SEQ changed
#define SP_XMIT_ESEQ_CHANGED  0x8

// Transmit because close
#define SP_XMIT_CLOSE         0x10

// Transmit because KA
#define SP_XMIT_KA            0x20


typedef struct mbus_seqpkt_con {

  net_task_t msc_task;

  uint8_t msc_remote_flags;
  uint8_t msc_local_flags;
  uint8_t msc_remote_addr;
  uint8_t msc_connection_id;

  uint8_t msc_seqgen;
  uint8_t msc_local_flags_sent;
  uint8_t msc_local_close;
  uint8_t msc_remote_close;

  uint8_t msc_txq_len;
  uint8_t msc_new_fragment;

  struct pbuf_queue msc_txq;
  struct pbuf_queue msc_rxq;
  LIST_ENTRY(mbus_seqpkt_con) msc_link;
  timer_t msc_ack_timer;
  timer_t msc_rtx_timer;
  timer_t msc_ka_timer;

  const service_t *msc_svc;
  void *msc_svc_opaque;

  int64_t msc_last_rx;

} mbus_seqpkt_con_t;

static void mbus_seqpkt_shutdown(mbus_seqpkt_con_t *msc, const char *reason);

static void mbus_seqpkt_rtx_timer(void *opaque, uint64_t expire);

static void mbus_seqpkt_ack_timer(void *opaque, uint64_t expire);

static void mbus_seqpkt_ka_timer(void *opaque, uint64_t expire);

static void mbus_seqpkt_task_cb(net_task_t *nt, uint32_t signals);

static void mbus_seqpkt_maybe_destroy(mbus_seqpkt_con_t *msc);


static void
mbus_seqpkt_service_event_cb(void *opaque, uint32_t events)
{
  mbus_seqpkt_con_t *msc = opaque;
  net_task_raise(&msc->msc_task, events);
}


static struct mbus_seqpkt_con_list mbus_seqpkt_cons;


static mbus_seqpkt_con_t *
find_connection(uint8_t src_addr, uint8_t connection_id)
{
  mbus_seqpkt_con_t *msc;
  LIST_FOREACH(msc, &mbus_seqpkt_cons, msc_link) {
    if(msc->msc_remote_addr == src_addr &&
       msc->msc_connection_id == connection_id)
      return msc;
  }
  return NULL;
}

static void
mbus_seqpkt_accept_err(const char *name, const char *reason,
                       uint8_t remote_addr)
{
  evlog(LOG_WARNING, "svc/%s: Remote %d unable to connect -- %s",
        name, remote_addr, reason);
}


static pbuf_t *
mbus_seqpkt_recv_connect(pbuf_t *pb, uint8_t src_addr)
{
  uint8_t *pkt = pbuf_data(pb, 0);
  size_t len = pb->pb_pktlen;

  pkt[len] = 0; // Zero-terminate service name (safe, CRC was here before)
  const char *name = (const char *)pkt + 2;

  evlog(LOG_INFO, "svc/%s: Connect from addr %d", name, src_addr);

  const service_t *s = service_find(name);
  if(s == NULL) {
    // TODO: Send reject reply
    mbus_seqpkt_accept_err(name, "Not available", src_addr);
    return pb;
  }

  mbus_seqpkt_con_t *msc = xalloc(sizeof(mbus_seqpkt_con_t), 0, MEM_MAY_FAIL);
  if(msc == NULL) {
    // TODO: Send reject reply
    mbus_seqpkt_accept_err(name, "No memory", src_addr);
    return pb;
  }
  memset(msc, 0, sizeof(mbus_seqpkt_con_t));
  msc->msc_task.nt_cb = mbus_seqpkt_task_cb;

  msc->msc_remote_addr = src_addr;
  msc->msc_connection_id = pkt[1];
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

  msc->msc_svc = s;

  msc->msc_svc_opaque = s->open(msc, mbus_seqpkt_service_event_cb,
                                MBUS_FRAGMENT_SIZE);

  msc->msc_last_rx = clock_get();

  if(msc->msc_svc_opaque == NULL) {
    free(msc);
    mbus_seqpkt_accept_err(name, "Failed to open", src_addr);
    // TODO: Send reject reply
    return pb;
  }

  LIST_INSERT_HEAD(&mbus_seqpkt_cons, msc, msc_link);

  pb = pbuf_trim(pb, len - 2);
  pkt[0] = msc->msc_local_flags;
  msc->msc_local_flags_sent = msc->msc_local_flags;

  net_timer_arm(&msc->msc_ka_timer, msc->msc_last_rx + SP_TIME_KA);

  return mbus_output_unicast(pb, msc->msc_remote_addr, 0);
}


static uint8_t
update_local_cts(mbus_seqpkt_con_t *msc)
{
  uint8_t prev = msc->msc_local_flags;
  if(msc->msc_svc_opaque != NULL && msc->msc_svc->may_push &&
     msc->msc_svc->may_push(msc->msc_svc_opaque)) {
    msc->msc_local_flags |= SP_CTS;
  } else {
    msc->msc_local_flags &= ~SP_CTS;
  }
  return (msc->msc_local_flags ^ prev) ? SP_XMIT_CTS_CHANGED : 0;
}



static pbuf_t *
pbuf_for_xmit(mbus_seqpkt_con_t *msc, pbuf_t *pb)
{
  if(pb == NULL) {
    pb = pbuf_make(2, 0);
    if(pb == NULL) {
      // No buffers available, retry soon
      net_timer_arm(&msc->msc_rtx_timer, clock_get() + SP_TIME_RTX);
      return NULL;
    }
  } else {
    pbuf_reset(pb, 2, 0);
  }
  return pb;
}

static void
tx_enq(mbus_seqpkt_con_t *msc, struct pbuf *pb)
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



static void
tx_pull(mbus_seqpkt_con_t *msc)
{
  if(msc->msc_local_close)
    return;

  if(msc->msc_svc_opaque == NULL)
    return;

  if(msc->msc_svc->pull == NULL)
    return;

  while(msc->msc_txq_len < 2) {

    pbuf_t *p = msc->msc_svc->pull(msc->msc_svc_opaque);
    if(p == NULL)
      break;

    //    pbuf_print("PULL", p, 1);
    tx_enq(msc, p);
  }
}



static pbuf_t *
mbus_seqpkt_output(mbus_seqpkt_con_t *msc, pbuf_t *pb, uint32_t xmit)
{
  if(msc->msc_remote_close)
    return pb;

  tx_pull(msc);

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

  xmit |= msc->msc_local_close ? SP_XMIT_CLOSE : 0;

  xmit |= update_local_cts(msc);

  if(!xmit)
    return pb;

  pb = pbuf_for_xmit(msc, pb);
  if(pb == NULL)
    return NULL;

  uint8_t *hdr = pbuf_append(pb, 2);
  hdr[0] = msc->msc_local_flags;

  if(tx != NULL && (xmit & (SP_XMIT_NEW_FRAGMENT | SP_XMIT_RTX))) {

    msc->msc_local_flags =
      (msc->msc_local_flags & ~SP_SEQ) | (tx->pb_flags & SP_SEQ);

    hdr[0] = msc->msc_local_flags | (tx->pb_flags & (SP_FF | SP_LF));

    if(STAILQ_NEXT(tx, pb_link))
      hdr[0] |= SP_MORE;

    uint8_t *payload = pbuf_append(pb, tx->pb_buflen);
    memcpy(payload, pbuf_cdata(tx, 0), tx->pb_buflen);

    net_timer_arm(&msc->msc_rtx_timer, clock_get() + SP_TIME_RTX);

  } else if(msc->msc_local_close) {

    const int last_seq = !!(msc->msc_local_flags_sent & SP_SEQ);
    const int expected_seq = !!(msc->msc_remote_flags & SP_ESEQ);

    if(last_seq != expected_seq) {
      hdr[0] = 0xc0; // Send close
      mbus_seqpkt_shutdown(msc, "Sent close");
    }
  }

#if 0
  char prefix[10];
  strlcpy(prefix,"TX       ", 10);
  prefix[3] = xmit & SP_XMIT_NEW_FRAGMENT ? 'N' : ' ';
  prefix[4] = xmit & SP_XMIT_RTX          ? 'R' : ' ';
  prefix[5] = xmit & SP_XMIT_CTS_CHANGED  ? 'c' : ' ';
  prefix[6] = xmit & SP_XMIT_ESEQ_CHANGED ? 'E' : ' ';
  prefix[7] = xmit & SP_XMIT_CLOSE        ? 'C' : ' ';
  prefix[8] = xmit & SP_XMIT_KA           ? 'K' : ' ';

  pbuf_print(prefix, pb, 1);
#endif
  timer_disarm(&msc->msc_ack_timer);

  hdr[1] = msc->msc_connection_id;
  msc->msc_local_flags_sent = hdr[0];
  return mbus_output_unicast(pb, msc->msc_remote_addr, 0);
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
  msc->msc_new_fragment = 1;
}


static pbuf_t *
mbus_seqpkt_recv_data(mbus_seqpkt_con_t *msc, pbuf_t *pb, uint8_t src_addr)
{
  uint8_t *pkt = pbuf_data(pb, 0);

  msc->msc_last_rx = clock_get();
  msc->msc_remote_flags = pkt[0];

  release_txq(msc);

  pb = pbuf_drop(pb, 2);

  if(pb->pb_pktlen) {

    const int recv_seq = !!(msc->msc_remote_flags & SP_SEQ);
    const int expect_seq = !!(msc->msc_local_flags & SP_ESEQ);

    if(recv_seq == expect_seq && msc->msc_local_flags & SP_CTS) {

      msc->msc_local_flags ^= SP_ESEQ;

      const int ack_time = msc->msc_remote_flags & SP_MORE ?
        SP_TIME_FAST_ACK : SP_TIME_ACK;
      net_timer_arm(&msc->msc_ack_timer, msc->msc_last_rx + ack_time);

      if(msc->msc_svc->push != NULL && msc->msc_svc_opaque != NULL) {

        STAILQ_INSERT_TAIL(&msc->msc_rxq, pb, pb_link);

        if(pb->pb_flags & PBUF_EOP) {
          pb = pbuf_splice(&msc->msc_rxq);
          pb = msc->msc_svc->push(msc->msc_svc_opaque, pb);
        }
      }
    }
  }

  pb = mbus_seqpkt_output(msc, pb, 0);
  mbus_seqpkt_maybe_destroy(msc);
  return pb;
}


static void
mbus_seqpkt_recv_close(mbus_seqpkt_con_t *msc, uint8_t src_addr,
                       const char *reason)
{
  msc->msc_remote_close = 1;
  mbus_seqpkt_shutdown(msc, reason);
  mbus_seqpkt_maybe_destroy(msc);
}


pbuf_t *
mbus_seqpkt_input(pbuf_t *pb, uint8_t src_addr)
{
  const uint8_t *pkt = pbuf_data(pb, 0);
  if(pb->pb_pktlen < 2)
    return pb;

  //  pbuf_print("RX", pb, 1);

  mbus_seqpkt_con_t *msc = find_connection(src_addr, pkt[1]);
  uint8_t flags = pkt[0];

  if(flags & 0x80) {
    // Handshake

    if(flags & 0x40) {

      // Close
      if(msc != NULL)
        mbus_seqpkt_recv_close(msc, src_addr, "Peer closed");

    } else {

      // Connect
      if(msc != NULL)
        mbus_seqpkt_recv_close(msc, src_addr, "Peer reconnected");

       pb =  mbus_seqpkt_recv_connect(pb, src_addr);
    }
  } else {
    // Data
    if(msc != NULL)
      pb = mbus_seqpkt_recv_data(msc, pb, src_addr);
  }

  return pb;
}


static void
mbus_seqpkt_maybe_destroy(mbus_seqpkt_con_t *msc)
{
  if(!msc->msc_local_close || msc->msc_svc_opaque)
    return;

  evlog(LOG_DEBUG, "svc/%s: Connection 0x%x from %d finalized",
        msc->msc_svc->name, msc->msc_connection_id, msc->msc_remote_addr);

  pbuf_free(STAILQ_FIRST(&msc->msc_rxq));
  pbuf_free(STAILQ_FIRST(&msc->msc_txq));
  timer_disarm(&msc->msc_rtx_timer);
  timer_disarm(&msc->msc_ack_timer);
  timer_disarm(&msc->msc_ka_timer);
  LIST_REMOVE(msc, msc_link);
  free(msc);
}


static void
mbus_seqpkt_task_cb(net_task_t *nt, uint32_t signals)
{
  mbus_seqpkt_con_t *msc = (mbus_seqpkt_con_t *)nt;

  if(signals & SERVICE_EVENT_CLOSE) {
    msc->msc_local_close = 1;
  }

  pbuf_t *pb = mbus_seqpkt_output(msc, NULL, 0);
  pbuf_free(pb);
  mbus_seqpkt_maybe_destroy(msc);
}


/**
 * This is called, either:
 *   1. If there is a timeout
 *   2. When we have received a close over the network
 *   3. When we send a close over the network
 */

static void
mbus_seqpkt_shutdown(mbus_seqpkt_con_t *msc, const char *reason)
{
  if(msc->msc_svc_opaque) {
    evlog(LOG_INFO, "svc/%s: Connection 0x%x from %d -- %s",
          msc->msc_svc->name, msc->msc_connection_id,
          msc->msc_remote_addr,  reason);
    msc->msc_svc->close(msc->msc_svc_opaque);
    msc->msc_svc_opaque = NULL;
  }
}



static void
mbus_seqpkt_timer(mbus_seqpkt_con_t *msc, uint32_t xmit)
{
  pbuf_t *pb = mbus_seqpkt_output(msc, NULL, xmit);
  pbuf_free(pb);
  mbus_seqpkt_maybe_destroy(msc);
}

static void
mbus_seqpkt_rtx_timer(void *opaque, uint64_t now)
{
  mbus_seqpkt_timer(opaque, SP_XMIT_RTX);
}


static void
mbus_seqpkt_ack_timer(void *opaque, uint64_t now)
{
  mbus_seqpkt_timer(opaque, SP_XMIT_ESEQ_CHANGED);
}


static void
mbus_seqpkt_ka_timer(void *opaque, uint64_t now)
{
  mbus_seqpkt_con_t *msc = opaque;

  if(now > msc->msc_last_rx + SP_TIME_TIMEOUT) {
    mbus_seqpkt_shutdown(msc, "Timeout");
  } else {
    net_timer_arm(&msc->msc_ka_timer, now + SP_TIME_KA);
    mbus_seqpkt_timer(msc, SP_XMIT_KA);
  }
}
