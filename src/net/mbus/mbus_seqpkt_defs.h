#pragma once

#include <stdint.h>
#include "mbus_flow.h"

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
#define SP_EOS  0x80

// We rely on these flags having the same value, so make sure that holds
_Static_assert(SP_FF  == PBUF_SOP);
_Static_assert(SP_LF  == PBUF_EOP);
_Static_assert(SP_SEQ == PBUF_SEQ);

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

LIST_HEAD(mbus_seqpkt_con_list, mbus_seqpkt_con);

typedef struct mbus_seqpkt_con {

  mbus_flow_t msc_flow;  // Must be first

  void *msc_app_opaque;
  const void *msc_app_vtable;

  net_task_t msc_task;

  uint8_t msc_remote_flags;
  uint8_t msc_local_flags;

  uint8_t msc_seqgen;
  uint8_t msc_local_flags_sent;

  uint8_t msc_app_closed;
  uint8_t msc_txq_len;

  uint8_t msc_new_fragment;
  uint8_t msc_rtx_attempt;

  struct pbuf_queue msc_txq;
  struct pbuf_queue msc_rxq;

  timer_t msc_ack_timer;
  timer_t msc_rtx_timer;
  timer_t msc_ka_timer;

  int64_t msc_last_rx;
  int64_t msc_last_tx;

  pbuf_t *(*msc_xmit)(pbuf_t *pb, struct mbus_seqpkt_con *msc);

  uint8_t (*msc_update_local_cts)(struct mbus_seqpkt_con *msc);

  int (*msc_prep_send)(struct mbus_seqpkt_con *msc);

  pbuf_t *(*msc_recv)(pbuf_t *pb, struct mbus_seqpkt_con *msc);

  void (*msc_shut_app)(struct mbus_seqpkt_con *msc, const char *reason);

  void (*msc_post_send)(struct mbus_seqpkt_con *msc);

  const char *msc_name;

  uint16_t msc_remote_xmit_credits;
  uint16_t msc_remote_avail_credits;

} mbus_seqpkt_con_t;


void mbus_seqpkt_txq_enq(mbus_seqpkt_con_t *msc, struct pbuf *pb);

void mbus_seqpkt_con_init(mbus_seqpkt_con_t *msc);
