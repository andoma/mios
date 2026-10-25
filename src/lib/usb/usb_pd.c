// USB Power Delivery: protocol layer and sink policy engine.
//
// The protocol layer runs in interrupt context: every received message
// must be acknowledged with GoodCRC within tTransmit (195 us). It also
// drops retransmissions (same MessageID) and hands the rest to the
// policy engine thread.
//
// The policy engine implements the sink side of an explicit contract:
// wait for Source_Capabilities, Request a fixed supply, wait for Accept
// and PS_RDY. Swaps and extended messages are refused, alternate modes
// (VDMs) are not supported.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mios/device.h>
#include <mios/error.h>
#include <mios/stream.h>
#include <mios/task.h>

#include <usb/usb_pd.h>

#include "irq.h"

// Message header
#define PD_HDR_TYPE(h)      ((h) & 0x1f)
#define PD_HDR_REV(h)       (((h) >> 6) & 3)
#define PD_HDR_MSGID(h)     (((h) >> 9) & 7)
#define PD_HDR_NDO(h)       (((h) >> 12) & 7)
#define PD_HDR_EXTENDED(h)  ((h) >> 15)

#define PD_REV20 1
#define PD_REV30 2

// Control messages
#define PD_CTRL_GOODCRC        1
#define PD_CTRL_ACCEPT         3
#define PD_CTRL_REJECT         4
#define PD_CTRL_PING           5
#define PD_CTRL_PS_RDY         6
#define PD_CTRL_GET_SOURCE_CAP 7
#define PD_CTRL_GET_SINK_CAP   8
#define PD_CTRL_DR_SWAP        9
#define PD_CTRL_PR_SWAP        10
#define PD_CTRL_VCONN_SWAP     11
#define PD_CTRL_WAIT           12
#define PD_CTRL_SOFT_RESET     13
#define PD_CTRL_NOT_SUPPORTED  16

// Data messages
#define PD_DATA_SOURCE_CAP     1
#define PD_DATA_REQUEST        2
#define PD_DATA_SINK_CAP       4
#define PD_DATA_VENDOR_DEFINED 15

// Power data objects
#define PDO_TYPE(p)            ((p) >> 30)
#define PDO_TYPE_FIXED         0
#define PDO_TYPE_BATTERY       1
#define PDO_TYPE_VARIABLE      2
#define PDO_TYPE_AUGMENTED     3
#define PDO_FIXED_MV(p)        ((((p) >> 10) & 0x3ff) * 50)
#define PDO_FIXED_MA(p)        (((p) & 0x3ff) * 10)

// Timers (us)
#define T_SINK_WAIT_CAP     465000  // tTypeCSinkWaitCap 310-620 ms
#define T_SENDER_RESPONSE    27000  // tSenderResponse 24-30 ms
#define T_PS_TRANSITION     500000  // tPSTransition 450-550 ms
#define T_SINK_REQUEST      100000  // After Wait
#define T_HARD_RESET_VBUS  3000000  // How long VBUS may be gone in Hard Reset
#define T_GOODCRC             3000  // Transmit plus tReceive (0.9-1.1 ms)

#define N_RETRY_COUNT    2
#define N_HARD_RESET     2

#define RXQ_SIZE 4

typedef enum {
  PE_IDLE,              // Not attached as sink
  PE_WAIT_CAPS,         // PE_SNK_Wait_for_Capabilities
  PE_SELECT,            // PE_SNK_Select_Capability, Request sent
  PE_TRANSITION,        // PE_SNK_Transition_Sink, Accepted
  PE_READY,             // PE_SNK_Ready, explicit contract
  PE_HARD_RESET,        // Waiting for the source to recover
  PE_DISABLED,          // Partner does not respond, Type-C only
} pe_state_t;

static const char *pe_state_names[] = {
  [PE_IDLE]       = "Idle",
  [PE_WAIT_CAPS]  = "Wait for capabilities",
  [PE_SELECT]     = "Select capability",
  [PE_TRANSITION] = "Transition",
  [PE_READY]      = "Ready",
  [PE_HARD_RESET] = "Hard reset",
  [PE_DISABLED]   = "Disabled (no PD partner)",
};

typedef enum {
  TX_IDLE,
  TX_SENDING,
  TX_WAIT_GOODCRC,
  TX_OK,
  TX_FAILED,
} tx_state_t;

typedef struct {
  uint8_t len;
  uint8_t buf[30];
} pd_msg_t;

struct usb_pd {
  device_t pd_dev;
  typec_phy_t *pd_phy;

  task_waitable_t pd_wait;

  // Protocol layer, protected by IRQ_LEVEL_IO
  uint8_t pd_active;
  uint8_t pd_rx_msgid;      // Last received MessageID, 0xff if none
  uint8_t pd_tx_msgid;      // MessageIDCounter
  uint8_t pd_rev;           // Spec revision to use in headers
  tx_state_t pd_tx_state;
  uint8_t pd_goodcrc_inflight; // Our GoodCRCs not yet completed
  uint8_t pd_hard_reset_rx;
  uint8_t pd_rxq_rd;
  uint8_t pd_rxq_wr;
  pd_msg_t pd_rxq[RXQ_SIZE];

  // Policy engine, owned by the thread
  uint8_t pd_attach_req;    // Set by usb_pd_attach/detach
  uint8_t pd_reevaluate;    // Policy changed
  pe_state_t pd_state;
  int64_t pd_deadline;
  int64_t pd_hard_reset_until;
  uint8_t pd_hard_resets;

  int pd_policy_mv;
  int pd_policy_ma;

  uint32_t pd_src_caps[7];
  uint8_t pd_num_src_caps;
  uint8_t pd_req_pos;       // Object position of the outstanding Request
  int pd_req_mv;
  int pd_req_ma;
  int pd_contract_mv;       // 0 if no explicit contract
  int pd_contract_ma;
  int pd_failed_mv;         // Lost the port going to this voltage, 0 if not

  uint32_t pd_rx_dropped;
  uint32_t pd_tx_retries;
  uint32_t pd_tx_failures;
  uint32_t pd_hard_resets_total;
  uint32_t pd_hard_resets_seen;
};


/******************************************************************
 * Protocol layer
 */

static uint16_t
pd_header(usb_pd_t *pd, int type, int ndo, int msgid)
{
  // Port power role sink (0), data role UFP (0)
  return type | (pd->pd_rev << 6) | (msgid << 9) | (ndo << 12);
}


static void
pd_phy_rx(void *opaque, const uint8_t *buf, size_t len)
{
  usb_pd_t *pd = opaque;
  if(!pd->pd_active || len < 2)
    return;

  const uint16_t hdr = buf[0] | (buf[1] << 8);
  const int msgid = PD_HDR_MSGID(hdr);

  if(PD_HDR_TYPE(hdr) == PD_CTRL_GOODCRC && PD_HDR_NDO(hdr) == 0 &&
     !PD_HDR_EXTENDED(hdr)) {
    if(pd->pd_tx_state == TX_WAIT_GOODCRC && msgid == pd->pd_tx_msgid) {
      pd->pd_tx_state = TX_OK;
      task_wakeup(&pd->pd_wait, 0);
    }
    return;
  }

  // Use the lower of our and the partner's revision
  const int rev = PD_HDR_REV(hdr);
  if(rev && rev < PD_REV30)
    pd->pd_rev = rev;

  uint8_t goodcrc[2];
  const uint16_t ack = pd_header(pd, PD_CTRL_GOODCRC, 0, msgid);
  goodcrc[0] = ack;
  goodcrc[1] = ack >> 8;
  pd->pd_goodcrc_inflight++;
  pd->pd_phy->tp_vtable->pd_transmit(pd->pd_phy, goodcrc, 2);

  const int soft_reset = PD_HDR_TYPE(hdr) == PD_CTRL_SOFT_RESET &&
    PD_HDR_NDO(hdr) == 0;
  if(!soft_reset && msgid == pd->pd_rx_msgid)
    return; // Retransmission, we've already got it
  pd->pd_rx_msgid = msgid;

  if((uint8_t)(pd->pd_rxq_wr - pd->pd_rxq_rd) == RXQ_SIZE) {
    pd->pd_rx_dropped++;
    return;
  }
  pd_msg_t *m = &pd->pd_rxq[pd->pd_rxq_wr++ & (RXQ_SIZE - 1)];
  m->len = len > sizeof(m->buf) ? sizeof(m->buf) : len;
  memcpy(m->buf, buf, m->len);
  task_wakeup(&pd->pd_wait, 0);
}


static void
pd_phy_tx_done(void *opaque, int status)
{
  usb_pd_t *pd = opaque;
  // The PHY completes transmissions in order
  if(pd->pd_goodcrc_inflight) {
    pd->pd_goodcrc_inflight--;
    return;
  }
  if(pd->pd_tx_state != TX_SENDING)
    return;
  pd->pd_tx_state = status ? TX_FAILED : TX_WAIT_GOODCRC;
  task_wakeup(&pd->pd_wait, 0);
}


static void
pd_phy_hard_reset(void *opaque, int received)
{
  usb_pd_t *pd = opaque;
  pd->pd_hard_resets_seen++;
  if(received && pd->pd_active) {
    pd->pd_hard_reset_rx = 1;
    task_wakeup(&pd->pd_wait, 0);
  }
}


static void
pd_protocol_reset(usb_pd_t *pd)
{
  int q = irq_forbid(IRQ_LEVEL_IO);
  pd->pd_rx_msgid = 0xff;
  pd->pd_tx_msgid = 0;
  pd->pd_tx_state = TX_IDLE;
  pd->pd_goodcrc_inflight = 0;
  irq_permit(q);
}


// Send a message and wait for GoodCRC, with retries. Thread context
static error_t
pd_send(usb_pd_t *pd, int type, int ndo, const uint32_t *objs)
{
  uint8_t buf[30];
  for(int i = 0; i < ndo; i++) {
    buf[2 + i * 4 + 0] = objs[i];
    buf[2 + i * 4 + 1] = objs[i] >> 8;
    buf[2 + i * 4 + 2] = objs[i] >> 16;
    buf[2 + i * 4 + 3] = objs[i] >> 24;
  }

  int q = irq_forbid(IRQ_LEVEL_IO);
  const uint16_t hdr = pd_header(pd, type, ndo, pd->pd_tx_msgid);
  buf[0] = hdr;
  buf[1] = hdr >> 8;

  error_t err = ERR_TX;
  for(int attempt = 0; attempt <= N_RETRY_COUNT && pd->pd_active;
      attempt++) {
    if(attempt)
      pd->pd_tx_retries++;
    pd->pd_tx_state = TX_SENDING;
    pd->pd_phy->tp_vtable->pd_transmit(pd->pd_phy, buf, 2 + ndo * 4);

    const int64_t deadline = clock_get() + T_GOODCRC;
    while(pd->pd_tx_state == TX_SENDING ||
          pd->pd_tx_state == TX_WAIT_GOODCRC) {
      if(task_sleep_deadline(&pd->pd_wait, deadline))
        break;
    }
    if(pd->pd_tx_state == TX_OK) {
      err = 0;
      break;
    }
  }
  pd->pd_tx_state = TX_IDLE;
  if(!err)
    pd->pd_tx_msgid = (pd->pd_tx_msgid + 1) & 7;
  else
    pd->pd_tx_failures++;
  irq_permit(q);
  return err;
}


static error_t
pd_send_ctrl(usb_pd_t *pd, int type)
{
  return pd_send(pd, type, 0, NULL);
}


/******************************************************************
 * Policy engine
 */

static void
pe_set_state(usb_pd_t *pd, pe_state_t state, int timeout)
{

  pd->pd_state = state;
  pd->pd_deadline = timeout ? clock_get() + timeout : 0;
}


static void
pe_hard_reset(usb_pd_t *pd, int send)
{
  pd->pd_contract_mv = 0;
  pd->pd_contract_ma = 0;
  pd->pd_hard_resets_total++;
  pd_protocol_reset(pd);

  if(send) {
    if(pd->pd_hard_resets >= N_HARD_RESET) {
      // The partner doesn't do PD (or doesn't like us), stay at 5V
      printf("%s: No response, giving up\n", pd->pd_dev.d_name);
      pe_set_state(pd, PE_DISABLED, 0);
      return;
    }
    pd->pd_hard_resets++;
    pd->pd_phy->tp_vtable->pd_hard_reset(pd->pd_phy);
  }

  // The source takes VBUS down and up again, then sends capabilities
  pd->pd_hard_reset_until = clock_get() + T_HARD_RESET_VBUS;
  pe_set_state(pd, PE_HARD_RESET, T_HARD_RESET_VBUS);
}


// Pick the highest fixed supply allowed by the policy. vSafe5V is always
// the first object, so there is always something
static void
pe_evaluate(usb_pd_t *pd)
{
  int best = 0;
  int best_mv = 0;
  for(int i = 0; i < pd->pd_num_src_caps; i++) {
    const uint32_t pdo = pd->pd_src_caps[i];
    if(PDO_TYPE(pdo) != PDO_TYPE_FIXED)
      continue;
    const int mv = PDO_FIXED_MV(pdo);
    if(pd->pd_failed_mv && mv >= pd->pd_failed_mv && mv > 5000)
      continue;
    if(mv <= pd->pd_policy_mv && mv > best_mv) {
      best = i;
      best_mv = mv;
    }
  }
  const int offered_ma = PDO_FIXED_MA(pd->pd_src_caps[best]);
  pd->pd_req_pos = best + 1;
  pd->pd_req_mv = best_mv;
  pd->pd_req_ma = offered_ma < pd->pd_policy_ma ? offered_ma :
    pd->pd_policy_ma;
}


static void
pe_send_request(usb_pd_t *pd)
{
  const uint32_t ma10 = pd->pd_req_ma / 10;
  const uint32_t rdo =
    (pd->pd_req_pos << 28) |
    (1 << 25) |               // USB communications capable
    (1 << 24) |               // No USB suspend
    (ma10 << 10) |            // Operating current
    ma10;                     // Maximum operating current

  if(pd_send(pd, PD_DATA_REQUEST, 1, &rdo)) {
    pe_hard_reset(pd, 1);
    return;
  }
  pe_set_state(pd, PE_SELECT, T_SENDER_RESPONSE);
}


static void
pe_send_sink_caps(usb_pd_t *pd)
{
  uint32_t pdos[2];
  int n = 0;
  const uint32_t ma10 = pd->pd_policy_ma / 10;

  pdos[n++] =
    (1 << 29) |               // Dual role power
    (1 << 26) |               // USB communications capable
    (1 << 25) |               // Dual role data
    (100 << 10) |             // 5V
    ma10;
  if(pd->pd_policy_mv >= 5050)
    pdos[n++] = ((pd->pd_policy_mv / 50) << 10) | ma10;

  pd_send(pd, PD_DATA_SINK_CAP, n, pdos);
}


static void
pe_not_supported(usb_pd_t *pd)
{
  pd_send_ctrl(pd, pd->pd_rev >= PD_REV30 ? PD_CTRL_NOT_SUPPORTED :
               PD_CTRL_REJECT);
}


static void
pe_handle_msg(usb_pd_t *pd, const pd_msg_t *m)
{
  const uint16_t hdr = m->buf[0] | (m->buf[1] << 8);
  const int type = PD_HDR_TYPE(hdr);
  const int ndo = PD_HDR_NDO(hdr);

  if(PD_HDR_EXTENDED(hdr)) {
    pe_not_supported(pd);
    return;
  }

  if(ndo) {
    switch(type) {
    case PD_DATA_SOURCE_CAP:
      pd->pd_num_src_caps = ndo;
      for(int i = 0; i < ndo; i++) {
        const uint8_t *o = m->buf + 2 + i * 4;
        pd->pd_src_caps[i] = o[0] | (o[1] << 8) | (o[2] << 16) | (o[3] << 24);
      }
      pd->pd_hard_resets = 0;
      pd->pd_hard_reset_until = 0;
      pe_evaluate(pd);
      pe_send_request(pd);
      break;
    case PD_DATA_VENDOR_DEFINED:
      break; // Not supported, ignored
    default:
      pe_not_supported(pd);
      break;
    }
    return;
  }

  switch(type) {
  case PD_CTRL_ACCEPT:
    if(pd->pd_state == PE_SELECT)
      pe_set_state(pd, PE_TRANSITION, T_PS_TRANSITION);
    break;

  case PD_CTRL_REJECT:
  case PD_CTRL_WAIT:
    if(pd->pd_state != PE_SELECT)
      break;
    if(pd->pd_contract_mv) {
      pe_set_state(pd, PE_READY, type == PD_CTRL_WAIT ? T_SINK_REQUEST : 0);
    } else {
      pe_set_state(pd, PE_WAIT_CAPS, T_SINK_WAIT_CAP);
    }
    break;

  case PD_CTRL_PS_RDY:
    if(pd->pd_state != PE_TRANSITION)
      break;
    pd->pd_contract_mv = pd->pd_req_mv;
    pd->pd_contract_ma = pd->pd_req_ma;
    pe_set_state(pd, PE_READY, 0);
    printf("%s: Contract %d mV %d mA\n", pd->pd_dev.d_name,
           pd->pd_contract_mv, pd->pd_contract_ma);
    break;

  case PD_CTRL_GET_SINK_CAP:
    pe_send_sink_caps(pd);
    break;

  case PD_CTRL_PING:
    break;

  case PD_CTRL_SOFT_RESET:
    pd_protocol_reset(pd);
    pd_send_ctrl(pd, PD_CTRL_ACCEPT);
    pd->pd_contract_mv = 0;
    pe_set_state(pd, PE_WAIT_CAPS, T_SINK_WAIT_CAP);
    break;

  case PD_CTRL_DR_SWAP:
  case PD_CTRL_PR_SWAP:
  case PD_CTRL_VCONN_SWAP:
    pd_send_ctrl(pd, PD_CTRL_REJECT);
    break;

  case PD_CTRL_NOT_SUPPORTED:
    break;

  default:
    pe_not_supported(pd);
    break;
  }
}


static void
pe_timeout(usb_pd_t *pd)
{
  switch(pd->pd_state) {
  case PE_WAIT_CAPS:
  case PE_SELECT:
  case PE_TRANSITION:
    pe_hard_reset(pd, 1);
    break;
  case PE_READY:
    // Retry after Wait
    pe_send_request(pd);
    break;
  case PE_HARD_RESET:
    // The source never came back. Type-C will detach us
    pe_set_state(pd, PE_WAIT_CAPS, T_SINK_WAIT_CAP);
    break;
  default:
    pd->pd_deadline = 0;
    break;
  }
}


__attribute__((noreturn))
static void *
pd_thread(void *arg)
{
  usb_pd_t *pd = arg;

  while(1) {
    int q = irq_forbid(IRQ_LEVEL_IO);
    while(!pd->pd_attach_req && !pd->pd_hard_reset_rx &&
          pd->pd_rxq_rd == pd->pd_rxq_wr && !pd->pd_reevaluate) {
      if(pd->pd_deadline) {
        if(task_sleep_deadline(&pd->pd_wait, pd->pd_deadline))
          break;
      } else {
        task_sleep(&pd->pd_wait);
      }
    }

    if(pd->pd_attach_req) {
      pd->pd_attach_req = 0;
      irq_permit(q);
      pd_protocol_reset(pd);
      pd->pd_contract_mv = 0;
      pd->pd_num_src_caps = 0;
      pd->pd_hard_resets = 0;
      pd->pd_hard_reset_until = 0;
      if(pd->pd_active)
        pe_set_state(pd, PE_WAIT_CAPS, T_SINK_WAIT_CAP);
      else
        pe_set_state(pd, PE_IDLE, 0);
      continue;
    }

    if(pd->pd_hard_reset_rx) {
      pd->pd_hard_reset_rx = 0;
      irq_permit(q);
      pe_hard_reset(pd, 0);
      continue;
    }

    if(pd->pd_rxq_rd != pd->pd_rxq_wr) {
      pd_msg_t m = pd->pd_rxq[pd->pd_rxq_rd++ & (RXQ_SIZE - 1)];
      irq_permit(q);
      if(pd->pd_state != PE_IDLE)
        pe_handle_msg(pd, &m);
      continue;
    }

    const int reevaluate = pd->pd_reevaluate;
    pd->pd_reevaluate = 0;
    irq_permit(q);

    if(reevaluate) {
      if(pd->pd_state == PE_READY && pd->pd_num_src_caps) {
        pe_evaluate(pd);
        if(pd->pd_req_mv != pd->pd_contract_mv ||
           pd->pd_req_ma != pd->pd_contract_ma)
          pe_send_request(pd);
      }
      continue;
    }

    if(pd->pd_deadline && clock_get() >= pd->pd_deadline)
      pe_timeout(pd);
  }
}


/******************************************************************
 * API
 */

void
usb_pd_attach(usb_pd_t *pd)
{
  int q = irq_forbid(IRQ_LEVEL_IO);
  pd->pd_rev = PD_REV30;
  pd->pd_active = 1;
  pd->pd_attach_req = 1;
  task_wakeup(&pd->pd_wait, 0);
  irq_permit(q);
  pd->pd_phy->tp_vtable->pd_rx_enable(pd->pd_phy, 1);
}


void
usb_pd_detach(usb_pd_t *pd)
{
  if(pd->pd_state == PE_TRANSITION && pd->pd_req_mv > 5000 &&
     pd->pd_req_mv != pd->pd_failed_mv) {
    // Something between the source and us can't take the new voltage.
    // Don't try again (until the policy changes) or we'd loop forever
    pd->pd_failed_mv = pd->pd_req_mv;
    printf("%s: Lost the port going to %d mV, not requesting that again\n",
           pd->pd_dev.d_name, pd->pd_req_mv);
  }
  pd->pd_phy->tp_vtable->pd_rx_enable(pd->pd_phy, 0);
  int q = irq_forbid(IRQ_LEVEL_IO);
  pd->pd_active = 0;
  pd->pd_attach_req = 1;
  pd->pd_rxq_rd = pd->pd_rxq_wr;
  task_wakeup(&pd->pd_wait, 0);
  irq_permit(q);
}


int
usb_pd_hard_reset_active(usb_pd_t *pd)
{
  return pd->pd_hard_reset_until && clock_get() < pd->pd_hard_reset_until;
}


void
usb_pd_set_sink_policy(usb_pd_t *pd, int max_mv, int max_ma)
{
  int q = irq_forbid(IRQ_LEVEL_IO);
  pd->pd_policy_mv = max_mv;
  pd->pd_policy_ma = max_ma;
  pd->pd_failed_mv = 0;
  pd->pd_reevaluate = 1;
  task_wakeup(&pd->pd_wait, 0);
  irq_permit(q);
}


static void
print_pdo(struct stream *st, int pos, uint32_t pdo)
{
  switch(PDO_TYPE(pdo)) {
  case PDO_TYPE_FIXED:
    stprintf(st, "  %d: Fixed %d mV %d mA\n", pos,
             PDO_FIXED_MV(pdo), PDO_FIXED_MA(pdo));
    break;
  case PDO_TYPE_BATTERY:
    stprintf(st, "  %d: Battery %d-%d mV %d mW\n", pos,
             ((pdo >> 10) & 0x3ff) * 50, ((pdo >> 20) & 0x3ff) * 50,
             (pdo & 0x3ff) * 250);
    break;
  case PDO_TYPE_VARIABLE:
    stprintf(st, "  %d: Variable %d-%d mV %d mA\n", pos,
             ((pdo >> 10) & 0x3ff) * 50, ((pdo >> 20) & 0x3ff) * 50,
             (pdo & 0x3ff) * 10);
    break;
  case PDO_TYPE_AUGMENTED:
    if(((pdo >> 28) & 3) == 0) {
      stprintf(st, "  %d: PPS %d-%d mV %d mA\n", pos,
               ((pdo >> 8) & 0xff) * 100, ((pdo >> 17) & 0xff) * 100,
               (pdo & 0x7f) * 50);
    } else {
      stprintf(st, "  %d: Augmented 0x%08x\n", pos, pdo);
    }
    break;
  }
}


static void
usb_pd_print_info(struct device *d, struct stream *st)
{
  usb_pd_t *pd = (usb_pd_t *)d;

  stprintf(st, "State: %s\n", pe_state_names[pd->pd_state]);
  stprintf(st, "Policy: up to %d mV, %d mA\n",
           pd->pd_policy_mv, pd->pd_policy_ma);
  if(pd->pd_failed_mv)
    stprintf(st, "Failed to get %d mV, staying below\n", pd->pd_failed_mv);
  if(pd->pd_contract_mv)
    stprintf(st, "Contract: %d mV %d mA (PD %s)\n",
             pd->pd_contract_mv, pd->pd_contract_ma,
             pd->pd_rev >= PD_REV30 ? "3.0" : "2.0");
  if(pd->pd_num_src_caps) {
    stprintf(st, "Source capabilities:\n");
    for(int i = 0; i < pd->pd_num_src_caps; i++)
      print_pdo(st, i + 1, pd->pd_src_caps[i]);
  }
  stprintf(st, "TX retries: %d  TX failures: %d  RX dropped: %d  "
           "Hard resets: %d\n",
           pd->pd_tx_retries, pd->pd_tx_failures, pd->pd_rx_dropped,
           pd->pd_hard_resets_total);
  stprintf(st, "Hard reset signaling seen: %d\n", pd->pd_hard_resets_seen);
}


static const device_class_t usb_pd_class = {
  .dc_class_name = "usb-pd",
  .dc_print_info = usb_pd_print_info,
};


usb_pd_t *
usb_pd_create(typec_phy_t *tp, const char *name)
{
  usb_pd_t *pd = calloc(1, sizeof(usb_pd_t));
  pd->pd_phy = tp;
  pd->pd_policy_mv = 5000;
  pd->pd_policy_ma = 500;
  pd->pd_rx_msgid = 0xff;

  pd->pd_dev.d_name = name;
  pd->pd_dev.d_class = &usb_pd_class;
  device_register(&pd->pd_dev);

  tp->tp_pd_opaque = pd;
  tp->tp_pd_rx = pd_phy_rx;
  tp->tp_pd_tx_done = pd_phy_tx_done;
  tp->tp_pd_hard_reset = pd_phy_hard_reset;

  thread_create(pd_thread, pd, 2048, name, 0, 6);
  return pd;
}
