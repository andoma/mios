// BLE advertising (non-connectable beacons) on SX1280
//
// Transmits ADV_NONCONN_IND on channels 37/38/39 as a radio scheduler
// slot. The whitening seed register is undocumented and loads
// bit-reversed relative to the BLE spec convention; sweep mode
// advertises seed candidates in the device name for empirical
// verification against a phone scanner.

#include "sx1280_i.h"
#include "sx1280_ble.h"
#include "sx1280_sched.h"

#include <mios/task.h>
#include <mios/eventlog.h>
#include <mios/type_macros.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "irq.h"
#include "net/pbuf.h"
#include "net/ble/l2cap.h"
#include "net/ble/ble.h"
#include "util/crc32.h"

#include <mios/sys.h>

#define BLE_ADV_PDU_MAX (2 + 6 + 31)
#define BLE_ADV_INTERVAL 100000 // µs, plus advDelay jitter

// Data channel PDU header bits
#define BLE_LLID_MASK   0x03
#define BLE_LLID_CONT   0x01 // L2CAP continuation / empty
#define BLE_LLID_START  0x02 // L2CAP start
#define BLE_LLID_CTRL   0x03
#define BLE_NESN        0x04
#define BLE_SN          0x08
#define BLE_MD          0x10

// LL control opcodes
#define LL_CONNECTION_UPDATE_IND 0x00
#define LL_CHANNEL_MAP_IND       0x01
#define LL_TERMINATE_IND         0x02
#define LL_UNKNOWN_RSP           0x07
#define LL_FEATURE_REQ           0x08
#define LL_FEATURE_RSP           0x09
#define LL_VERSION_IND           0x0c
#define LL_PING_REQ              0x12
#define LL_PING_RSP              0x13
#define LL_LENGTH_REQ            0x14
#define LL_LENGTH_RSP            0x15

// AutoTx fires at a fixed offset after RX end plus the programmed
// time; the datasheet example claims a 33µs offset but the silicon
// disagrees. Calibrated per master (arm-value sweep watching for the
// incoming NESN flip, `sx1280 ble atx <n>`): a phone acks 19-22, a
// Realtek RTL8761 dongle 62-82. Default to the Realtek band; the
// phone's upper bound is untested and the two may yet overlap here.
#define BLE_AUTOTX_TIME 72

#define BLE_CONN_LEAD    1000 // Fire the event slot this early, µs
#define BLE_CONN_TX_BASE 0x80 // TX half of the 256B data buffer

// Shared modem-config token for advertising
static const char ble_mode[] = "ble";

typedef struct {
  sx1280_slot_t slot;
  sx1280_t *chip;

  uint32_t tx_done;
  uint32_t tx_timeout;
  uint32_t cmd_errors;
  uint32_t rx_scan_req;
  uint32_t rx_conn_ind;
  uint32_t rx_other;
  uint32_t rx_crc_errors;
  uint32_t rx_misaddr;     // SCAN_REQ/CONNECT_IND not addressed to us
  uint32_t max_turnaround; // µs, TxDone -> SetRx accepted

  uint8_t sweep;      // Sweep whitening seeds, name carries the seed
  uint8_t sweep_seed;
  uint8_t sweep_cnt;
  uint8_t ch;         // Next advertising channel index (one per slot)
  uint8_t tmo_streak; // Consecutive TX timeouts -> radio recover

  char name[17];
} sx1280_ble_adv_t;

// Our static random device address, derived from the chip's unique
// id so every board gets its own. Stored LSB first (transmit order);
// the top two bits of the last byte mark it as static random.
static uint8_t ble_our_addr[6];

static void
ble_addr_init(void)
{
  if(ble_our_addr[5])
    return; // Already derived (byte 5 is always >= 0xc0)

  const struct serial_number sn = sys_get_serial_number();
  const uint32_t a = crc32(0, sn.data, sn.len);
  const uint32_t b = crc32(a, sn.data, sn.len);
  ble_our_addr[0] = a;
  ble_our_addr[1] = a >> 8;
  ble_our_addr[2] = a >> 16;
  ble_our_addr[3] = a >> 24;
  ble_our_addr[4] = b;
  ble_our_addr[5] = 0xc0 | (b >> 8);
}

static sx1280_ble_adv_t *g_adv;

// --- Connection (peripheral role) ---

#define BLE_LL_MAX_PAYLOAD 27  // Pre-DLE default per BT spec
// With DLE negotiated: bounded by the SX1280's 128-byte buffer halves
// (TX at 0x80, RX at 0) minus the 2-byte PDU header
#define BLE_LL_DLE_PAYLOAD 126
#define BLE_LL_TIME(octets) (((octets) + 10) * 8) // 1M PHY airtime, µs

typedef struct {
  sx1280_slot_t slot;
  sx1280_t *chip;

  l2cap_t l2c;            // mios BLE host stack attachment

  uint32_t access_addr;
  uint32_t crc_init;

  int64_t anchor;         // Where the master may start transmitting
  int64_t last_sync;      // Last anchor resync (for window widening)
  int64_t last_rx;        // For supervision timeout
  uint32_t interval;      // µs
  uint32_t timeout;       // µs
  uint32_t window;        // Listen slack after anchor (first event /
                          // connection update), µs
  uint16_t event_counter;

  uint8_t active;
  uint8_t established;    // Received at least one packet
  uint8_t term_code;
  uint8_t version_sent;
  uint8_t autotx_time;    // SetAutoTx arm value (calibration sweep)
  uint8_t param_req_sent; // L2CAP conn param update request sent
  uint8_t length_req_sent;// LL_LENGTH_REQ initiated by us
  uint8_t term_req;       // Local termination requested (any thread)
  uint8_t term_sent;      // LL_TERMINATE_IND queued
  uint8_t coll_missed;    // Consecutive missed events (fairness boost)
  uint8_t phase;          // Debug: event stage for recover diagnosis
  uint8_t recovers;       // Chip resets survived by this connection
  uint8_t eff_tx;         // Effective max TX payload (DLE negotiated)
  uint8_t rx_accept;      // pick_tx's ack/NAK decision for this PDU
  struct pbuf *rx_spare;  // Pre-reserved buffer; NULL means NAK data
  int64_t term_deadline;  // Give up if the ack never arrives

  uint8_t chmap[37];      // CSA#1 remap table
  uint8_t last_unmapped_channel;
  uint8_t hop_increment;

  uint8_t pending_chmask[5];
  uint16_t pending_chmask_instant;
  uint8_t pending_chmask_valid;

  uint8_t pending_update[9]; // WinSize,WinOffset2,Interval2,Lat2,Timo2
  uint16_t pending_update_instant;
  uint8_t pending_update_valid;

  // ARQ state
  uint8_t last_rx_sn;     // BLE_SN bit of last accepted packet
  uint8_t tx_seq;         // Our sequence (0/1)
  uint8_t tx_dummy;       // Last transmitted PDU was an empty one

  // Sub-fragmentation of an oversized l2cap fragment across LL PDUs
  uint16_t tx_frag_off;

  // The in-flight (unacked) LL PDU, copied out of the queue so queue
  // and pbuf lifetime stay simple
  uint8_t tx_pdu[2 + BLE_LL_DLE_PAYLOAD];
  uint8_t tx_src;         // 0 = empty PDU, 1 = ctrl ring, 2 = l2cap queue

  // Control responses take priority over l2cap data
#define BLE_CTRLQ_SIZE 4
#define BLE_CTRLQ_PDU  12
  uint8_t ctrlq[BLE_CTRLQ_SIZE][BLE_CTRLQ_PDU];
  uint8_t ctrlq_head, ctrlq_count;

  // Stats
  uint32_t ev_rx, ev_missed, ev_crc;
  uint32_t rx_pdus;       // Total PDUs (several per event with MD)
  uint32_t tx_acked, tx_retrans, rx_bad_seq, rx_data, rx_drops;
  uint32_t tx_fired, tx_nofire; // AutoTx TxDone seen / not seen
  uint32_t txdone_pin_miss;           // TX_DONE set but the pin never rose
  uint32_t max_patch;     // µs, RxDone -> response PDU written

  uint8_t peer_addr[6];
} ble_conn_t;

// Connection pool: peripheral links to multiple centrals, each with
// its own anchor timeline, ARQ state and l2cap instance. The arbiter
// interleaves their events (and advertising) by time and priority.
#define BLE_MAX_CONN 2
static ble_conn_t *g_conns[BLE_MAX_CONN];

#define BLE_CONN_PRIO 3 // Base slot priority (advertising runs at 1)

static ble_conn_t *
ble_conn_from_l2c(struct l2cap *l2c)
{
  return (ble_conn_t *)((char *)l2c - offsetof(ble_conn_t, l2c));
}

// CLI-tunables for calibration against different masters
static uint8_t g_ble_autotx_time = BLE_AUTOTX_TIME;

// Advertised DLE payload cap (LL_LENGTH_REQ/RSP maxRx and maxTx).
// Some masters misbehave with long PDUs even after negotiating them
// (Zephyr's split LL central skips whole connection events); capping
// what we advertise bounds PDU airtime in both directions.
static uint8_t g_ble_dle_max = BLE_LL_DLE_PAYLOAD;
static int8_t g_ble_tx_dbm = 10;

// Channel index -> MHz above 2400 (BLE data channels 0-36)
static const uint8_t ble_ch_freq[37] = {
   4,  6,  8, 10, 12, 14, 16, 18, 20, 22,
  24, 28, 30, 32, 34, 36, 38, 40, 42, 44,
  46, 48, 50, 52, 54, 56, 58, 60, 62, 64,
  66, 68, 70, 72, 74, 76, 78,
};

// Build the CSA#1 remap table: chmap[i] = i for used channels,
// remap into the used set otherwise
static void
ble_conn_update_channels(uint8_t *chmap, const uint8_t *mask)
{
  int num_used = 0;
  for(int i = 0; i <= 36; i++) {
    if(mask[i / 8] & (1 << (i & 7)))
      chmap[num_used++] = i;
  }
  if(num_used == 0)
    return; // Malformed; keep previous table

  for(int i = 36; i >= 0; i--) {
    if(mask[i / 8] & (1 << (i & 7)))
      chmap[i] = i;
    else
      chmap[i] = chmap[i % num_used];
  }
}

static uint8_t *
ble_conn_enqueue_ctrl(ble_conn_t *c, uint8_t op, uint8_t len)
{
  if(c->ctrlq_count == BLE_CTRLQ_SIZE)
    return NULL; // Full; peer retransmits its request later

  uint8_t *pdu = c->ctrlq[(c->ctrlq_head + c->ctrlq_count) % BLE_CTRLQ_SIZE];
  c->ctrlq_count++;
  pdu[0] = BLE_LLID_CTRL;
  pdu[1] = len + 1;
  pdu[2] = op;
  return pdu + 3;
}

static void
ble_conn_fill_length(uint8_t *p)
{
  const uint16_t o = g_ble_dle_max;
  const uint16_t t = BLE_LL_TIME(o);
  p[0] = o & 0xff;
  p[1] = o >> 8;
  p[2] = t & 0xff;
  p[3] = t >> 8;
  p[4] = o & 0xff;
  p[5] = o >> 8;
  p[6] = t & 0xff;
  p[7] = t >> 8;
}

static void
ble_conn_handle_ctrl(ble_conn_t *c, const uint8_t *req, int len)
{
  uint8_t *rsp;

  if(len < 1)
    return;

  switch(req[0]) {
  case LL_FEATURE_REQ:
    if((rsp = ble_conn_enqueue_ctrl(c, LL_FEATURE_RSP, 8)) != NULL) {
      memset(rsp, 0, 8);
      rsp[0] = 0x20; // LE Data Packet Length Extension
    }
    break;

  case LL_VERSION_IND:
    if(!c->version_sent &&
       (rsp = ble_conn_enqueue_ctrl(c, LL_VERSION_IND, 5)) != NULL) {
      c->version_sent = 1;
      rsp[0] = 0x08;   // Core 4.2
      rsp[1] = 0xff;   // CompId: no assigned number
      rsp[2] = 0xff;
      rsp[3] = 0x00;
      rsp[4] = 0x00;
    }
    break;

  case LL_CHANNEL_MAP_IND:
    if(len >= 8) {
      memcpy(c->pending_chmask, req + 1, 5);
      c->pending_chmask_instant = req[6] | (req[7] << 8);
      c->pending_chmask_valid = 1;
    }
    break;

  case LL_CONNECTION_UPDATE_IND:
    if(len >= 12) {
      memcpy(c->pending_update, req + 1, 9);
      c->pending_update_instant = req[10] | (req[11] << 8);
      c->pending_update_valid = 1;
    }
    break;

  case LL_TERMINATE_IND:
    c->term_code = len >= 2 ? req[1] : 0x13;
    break;

  case LL_LENGTH_REQ:
  case LL_LENGTH_RSP:
    // MaxRxOctets(2) MaxRxTime(2) MaxTxOctets(2) MaxTxTime(2).
    // No instant: effective lengths apply right away. Our TX is
    // bounded by what the peer can receive, in octets and in time.
    if(len >= 9) {
      const uint16_t peer_rx_octets = req[1] | (req[2] << 8);
      const uint16_t peer_rx_time = req[3] | (req[4] << 8);
      uint16_t tx = g_ble_dle_max;
      if(tx > peer_rx_octets)
        tx = peer_rx_octets;
      if(BLE_LL_TIME(tx) > peer_rx_time)
        tx = peer_rx_time / 8 - 10;
      if(tx < BLE_LL_MAX_PAYLOAD)
        tx = BLE_LL_MAX_PAYLOAD;
      c->eff_tx = tx;
      evlog(LOG_NOTICE, "%s: BLE data length: op:%02x peer rx %d octets "
            "%d us -> tx %d octets", c->chip->name, req[0],
            peer_rx_octets, peer_rx_time, c->eff_tx);
    }
    if(req[0] == LL_LENGTH_REQ &&
       (rsp = ble_conn_enqueue_ctrl(c, LL_LENGTH_RSP, 8)) != NULL)
      ble_conn_fill_length(rsp);
    break;

  case LL_UNKNOWN_RSP:
    break; // Never answer an unknown-response with another one

  case LL_PING_REQ:
    ble_conn_enqueue_ctrl(c, LL_PING_RSP, 0);
    break;

  default:
    if((rsp = ble_conn_enqueue_ctrl(c, LL_UNKNOWN_RSP, 1)) != NULL)
      rsp[0] = req[0];
    break;
  }
}

// l2cap hands us raw fragments (PBUF_SOP marks an SDU start); the
// radio thread drains the queue one LL PDU chunk per connection event
static void
ble_conn_l2cap_output(struct l2cap *self, struct pbuf *pb)
{
  ble_conn_t *c = ble_conn_from_l2c(self);

  if(pb == NULL) {
    // l2cap layer closed
    self->l2c_output = NULL;
    return;
  }

  if(pbuf_pullup(pb, pb->pb_pktlen))
    panic("%s: pullup failed", __FUNCTION__);

  int q = irq_forbid(IRQ_LEVEL_NET);
  if(c != NULL && c->active) {
    STAILQ_INSERT_TAIL(&self->l2c_tx_queue, pb, pb_link);
    self->l2c_tx_queue_len++;
  } else {
    pbuf_free_irq_blocked(pb);
  }
  irq_permit(q);
}

static int64_t
ble_conn_drop(ble_conn_t *c, sx1280_t *s, uint8_t code, const char *why)
{
  evlog(LOG_NOTICE, "%s: BLE disconnected (0x%02x, %s) "
        "rx:%d missed:%d crc:%d acked:%d retrans:%d "
        "txfired:%d nofire:%d data:%d patch:%dus",
        s->name, code, why, (int)c->ev_rx, (int)c->ev_missed,
        (int)c->ev_crc, (int)c->tx_acked, (int)c->tx_retrans,
        (int)c->tx_fired, (int)c->tx_nofire, (int)c->rx_data,
        (int)c->max_patch);

  c->active = 0;

  l2cap_disconnect(&c->l2c);

  int q = irq_forbid(IRQ_LEVEL_NET);
  pbuf_free_queue_irq_blocked(&c->l2c.l2c_tx_queue);
  c->l2c.l2c_tx_queue_len = 0;
  if(c->rx_spare != NULL) {
    pbuf_free_irq_blocked(c->rx_spare);
    c->rx_spare = NULL;
  }
  irq_permit(q);

  sx1280_sched_set_mode(s, NULL);
  return 0; // Advertising runs independently of connections
}

// Advance the TX pipeline after an acknowledgement: pop the acked
// control PDU or consume the acked chunk of the l2cap queue head
static void
ble_conn_tx_consume(ble_conn_t *c)
{
  if(c->tx_src == 1) {
    // An acknowledged LL_TERMINATE_IND completes the termination
    // procedure (BT spec 5.1.6)
    if(c->ctrlq[c->ctrlq_head][2] == LL_TERMINATE_IND)
      c->term_code = c->ctrlq[c->ctrlq_head][3];
    c->ctrlq_head = (c->ctrlq_head + 1) % BLE_CTRLQ_SIZE;
    c->ctrlq_count--;
  } else if(c->tx_src == 2) {
    int q = irq_forbid(IRQ_LEVEL_NET);
    pbuf_t *pb = STAILQ_FIRST(&c->l2c.l2c_tx_queue);
    if(pb != NULL) {
      c->tx_frag_off += c->tx_pdu[1];
      if(c->tx_frag_off >= pb->pb_pktlen) {
        STAILQ_REMOVE_HEAD(&c->l2c.l2c_tx_queue, pb_link);
        c->l2c.l2c_tx_queue_len--;
        pb->pb_next = NULL;
        pbuf_free_irq_blocked(pb);
        c->tx_frag_off = 0;
      }
    }
    irq_permit(q);
  }
}

// Load the next PDU to transmit into tx_pdu: control responses first,
// then (a chunk of) the l2cap TX queue head, else an empty PDU
static void
ble_conn_tx_load(ble_conn_t *c)
{
  if(c->ctrlq_count) {
    const uint8_t *pdu = c->ctrlq[c->ctrlq_head];
    memcpy(c->tx_pdu, pdu, 2 + pdu[1]);
    c->tx_src = 1;
    return;
  }

  int q = irq_forbid(IRQ_LEVEL_NET);
  pbuf_t *pb = STAILQ_FIRST(&c->l2c.l2c_tx_queue);
  if(pb != NULL) {
    const uint8_t *d = pbuf_data(pb, 0);
    const uint16_t left = pb->pb_pktlen - c->tx_frag_off;
    const uint8_t chunk = left > c->eff_tx ? c->eff_tx : left;

    c->tx_pdu[0] = c->tx_frag_off ? BLE_LLID_CONT :
      ((pb->pb_flags & PBUF_SOP) ? BLE_LLID_START : BLE_LLID_CONT);
    c->tx_pdu[1] = chunk;
    memcpy(c->tx_pdu + 2, d + c->tx_frag_off, chunk);
    c->tx_src = 2;
    irq_permit(q);
    return;
  }
  irq_permit(q);

  c->tx_pdu[0] = BLE_LLID_CONT;
  c->tx_pdu[1] = 0;
  c->tx_src = 0;
}

// More TX queued beyond the PDU currently in tx_pdu?
static int
ble_conn_tx_pending(ble_conn_t *c)
{
  if(c->ctrlq_count > (c->tx_src == 1 ? 1 : 0))
    return 1;

  int more = 0;
  int q = irq_forbid(IRQ_LEVEL_NET);
  pbuf_t *pb = STAILQ_FIRST(&c->l2c.l2c_tx_queue);
  if(pb != NULL) {
    if(c->tx_src == 2)
      more = c->tx_frag_off + c->tx_pdu[1] < pb->pb_pktlen ||
        STAILQ_NEXT(pb, pb_link) != NULL;
    else
      more = 1;
  }
  irq_permit(q);
  return more;
}

// ARQ + next TX selection, per BT spec 4.5.9. Fills tx_pdu and
// returns the on-air header byte.
static void
ble_conn_pick_tx(ble_conn_t *c, uint8_t rx_b0, uint8_t rx_len,
                 uint8_t *hdr_out, uint8_t *len_out)
{
  // Did the peer acknowledge our previous PDU?
  const uint8_t nesn = !!(rx_b0 & BLE_NESN);
  if(c->tx_seq != nesn) {
    if(c->tx_src) {
      ble_conn_tx_consume(c);
      c->tx_acked++;
    }
    c->tx_seq = nesn;
    ble_conn_tx_load(c);
  } else {
    if(c->tx_src)
      c->tx_retrans++; // Same PDU goes out again, bits repatched
    else
      ble_conn_tx_load(c); // Unheard empty PDU may be upgraded
  }
  c->tx_dummy = (c->tx_src == 0);

  // The SN of the incoming packet decides our NESN: acknowledge it
  // if it is the one we expected AND, for data PDUs, an RX buffer is
  // reserved. NAK (NESN unchanged) makes pbuf exhaustion lossless:
  // the peer retransmits instead of us acking into the void.
  int accept = (c->last_rx_sn ^ rx_b0) & BLE_SN;
  if(accept && (rx_b0 & BLE_LLID_MASK) != BLE_LLID_CTRL &&
     rx_len > 0 && c->rx_spare == NULL)
    accept = 0;
  c->rx_accept = accept;

  const uint8_t next_rx_sn =
    accept ? (rx_b0 & BLE_SN) : c->last_rx_sn;

  uint8_t b0 = c->tx_pdu[0] & BLE_LLID_MASK;
  if(!next_rx_sn)
    b0 |= BLE_NESN;
  if(c->tx_seq)
    b0 |= BLE_SN;
  if(ble_conn_tx_pending(c))
    b0 |= BLE_MD; // Ask the master to keep this event going

  *hdr_out = b0;
  *len_out = c->tx_pdu[1];
}

// 2402, 2426, 2480 MHz
static const uint8_t adv_channels[3] = {37, 38, 39};
static const uint32_t adv_freq[3] = {2402000000u, 2426000000u, 2480000000u};

static uint8_t
bitrev7(uint8_t v)
{
  uint8_t r = 0;
  for(int i = 0; i < 7; i++)
    if(v & (1 << i))
      r |= 1 << (6 - i);
  return r;
}

static error_t
ble_radio_setup(sx1280_t *s)
{
  error_t err;

  static const uint8_t standby[2] = {SX1280_SET_STANDBY, SX1280_STDBY_RC};
  if((err = sx1280_cmd(s, standby, NULL, sizeof(standby))) != 0)
    return err;

  static const uint8_t pkttype[2] = {SX1280_SET_PACKETTYPE,
                                     SX1280_PACKET_TYPE_BLE};
  if((err = sx1280_cmd(s, pkttype, NULL, sizeof(pkttype))) != 0)
    return err;

  static const uint8_t modparams[4] = {SX1280_SET_MODULATIONPARAMS,
                                       SX1280_BLE_BR_1_000_BW_1_2,
                                       SX1280_BLE_MOD_IND_0_5,
                                       SX1280_BLE_BT_0_5};
  if((err = sx1280_cmd(s, modparams, NULL, sizeof(modparams))) != 0)
    return err;

  static const uint8_t pktparams[8] = {SX1280_SET_PACKETPARAMS,
                                       SX1280_BLE_PAYLOAD_MAX_255,
                                       SX1280_BLE_CRC_3B,
                                       0, // test payload, unused
                                       SX1280_BLE_WHITENING_ENABLE,
                                       0, 0, 0};
  if((err = sx1280_cmd(s, pktparams, NULL, sizeof(pktparams))) != 0)
    return err;

  static const uint8_t baseaddr[3] = {SX1280_SET_BUFFERBASEADDRESS, 0, 0};
  if((err = sx1280_cmd(s, baseaddr, NULL, sizeof(baseaddr))) != 0)
    return err;

  static const uint8_t aa[4] = {0x8e, 0x89, 0xbe, 0xd6};
  if((err = sx1280_write_reg(s, SX1280_REG_BLE_ACCESS_ADDR, aa, 4)) != 0)
    return err;

  static const uint8_t crcinit[3] = {0x55, 0x55, 0x55};
  if((err = sx1280_write_reg(s, SX1280_REG_CRC_INIT, crcinit, 3)) != 0)
    return err;

  // value = dBm + 18; 2µs ramp (the ramp delays AutoTx and the T_IFS
  // budget is ±2µs)
  const uint8_t txparams[3] = {SX1280_SET_TXPARAMS,
                               g_ble_tx_dbm + 18, 0x00};
  if((err = sx1280_cmd(s, txparams, NULL, sizeof(txparams))) != 0)
    return err;

  // Park in FS between TX/RX: shortens the T_IFS turnaround
  static const uint8_t autofs[2] = {SX1280_SET_AUTOFS, 1};
  if((err = sx1280_cmd(s, autofs, NULL, sizeof(autofs))) != 0)
    return err;

  // Disarm AutoTx (may be armed from a previous connection)
  static const uint8_t autotx_off[3] = {SX1280_SET_AUTOTX, 0, 0};
  if((err = sx1280_cmd(s, autotx_off, NULL, sizeof(autotx_off))) != 0)
    return err;

  // Chip DIO2 (if wired) carries TX_DONE alone: a clean interrupt for
  // the AutoTx completion that nothing needs to ClrIrqStatus for
  const uint16_t irqmask = SX1280_IRQ_TX_DONE | SX1280_IRQ_RX_DONE |
    SX1280_IRQ_CRC_ERROR | SX1280_IRQ_RX_TX_TIMEOUT;
  const uint8_t dioirq[9] = {SX1280_SET_DIOIRQPARAMS,
                             irqmask >> 8, irqmask & 0xff,
                             irqmask >> 8, irqmask & 0xff, // DIO1
                             0, SX1280_IRQ_TX_DONE,        // DIO2
                             0, 0};                        // DIO3
  return sx1280_cmd(s, dioirq, NULL, sizeof(dioirq));
}

// Per-event radio configuration for a connection event on channel ch.
// Everything connection-specific is rewritten each event so no state
// is assumed across mode switches.
static error_t
ble_conn_config_event(ble_conn_t *c, sx1280_t *s, uint8_t ch)
{
  error_t err;

  // AutoTx must be armed from STDBY_RC
  c->phase = 20;
  static const uint8_t standby[2] = {SX1280_SET_STANDBY, SX1280_STDBY_RC};
  if((err = sx1280_cmd(s, standby, NULL, sizeof(standby))) != 0)
    return err;

  // AutoFS stays on: the post-TX FS parking gives the fast TX->RX
  // turnaround that multi-PDU events need. (The AutoTx cancellations
  // once blamed on AutoFS were the mid-countdown ClrIrqStatus.)
  c->phase = 21;
  const uint8_t autotx[3] = {SX1280_SET_AUTOTX, 0, c->autotx_time};
  if((err = sx1280_cmd(s, autotx, NULL, sizeof(autotx))) != 0)
    return err;

  c->phase = 22;
  const uint8_t aa[4] = {c->access_addr >> 24, c->access_addr >> 16,
                         c->access_addr >> 8, c->access_addr};
  if((err = sx1280_write_reg(s, SX1280_REG_BLE_ACCESS_ADDR, aa, 4)) != 0)
    return err;

  c->phase = 23;
  const uint8_t crc[3] = {c->crc_init >> 16, c->crc_init >> 8, c->crc_init};
  if((err = sx1280_write_reg(s, SX1280_REG_CRC_INIT, crc, 3)) != 0)
    return err;

  c->phase = 24;
  static const uint8_t baseaddr[3] = {SX1280_SET_BUFFERBASEADDRESS,
                                      BLE_CONN_TX_BASE, 0};
  if((err = sx1280_cmd(s, baseaddr, NULL, sizeof(baseaddr))) != 0)
    return err;

  c->phase = 25;
  const uint32_t f =
    SX1280_HZ_TO_FREQ((2400 + ble_ch_freq[ch]) * 1000000u);
  const uint8_t freq[4] = {SX1280_SET_RFFREQUENCY, f >> 16, f >> 8, f};
  if((err = sx1280_cmd(s, freq, NULL, sizeof(freq))) != 0)
    return err;

  c->phase = 26;
  const uint8_t seed = bitrev7(0x40 | ch);
  if((err = sx1280_write_reg(s, SX1280_REG_WHITENING_SEED, &seed, 1)) != 0)
    return err;

  // Fallback response with predicted ARQ bits, in case the post-RX
  // patch path ever runs late: an empty PDU is always protocol-legal
  c->phase = 27;
  uint8_t fb[4] = {SX1280_WRITE_BUFFER, BLE_CONN_TX_BASE,
                   BLE_LLID_CONT, 0};
  if(!c->last_rx_sn)
    fb[2] |= BLE_NESN;
  if(c->tx_seq)
    fb[2] |= BLE_SN;
  if((err = sx1280_cmd(s, fb, NULL, sizeof(fb))) != 0)
    return err;

  c->phase = 28;
  return sx1280_irq_clear(s);
}

// Process the received PDU payload (after the T_IFS-critical path)
static error_t
ble_conn_process_rx(ble_conn_t *c, sx1280_t *s, uint8_t b0, uint8_t rxlen)
{
  if(!c->rx_accept) {
    // Duplicate SN, or a data PDU NAKed for want of an RX buffer
    // (counted separately); the peer retransmits
    if((c->last_rx_sn ^ b0) & BLE_SN)
      c->rx_drops++;
    else
      c->rx_bad_seq++;
    return 0;
  }
  c->last_rx_sn = b0 & BLE_SN;

  if(rxlen == 0 || rxlen > BLE_LL_DLE_PAYLOAD)
    goto refill; // Empty PDU (or filtered oversize)

  uint8_t buf[3 + 2 + BLE_LL_DLE_PAYLOAD];
  buf[0] = SX1280_READ_BUFFER;
  buf[1] = 0;
  buf[2] = 0;
  error_t err = sx1280_cmd(s, buf, buf, 3 + 2 + rxlen);
  if(err)
    return err;

  const uint8_t *payload = buf + 3 + 2;

  switch(b0 & BLE_LLID_MASK) {
  case BLE_LLID_CTRL:
    ble_conn_handle_ctrl(c, payload, rxlen);
    break;

  case BLE_LLID_START:
  case BLE_LLID_CONT: {
    // The buffer was reserved before we acked (pick_tx NAKs data
    // PDUs otherwise), so this cannot fail. Same layout the other LL
    // drivers hand to l2cap_input: payload at offset 2.
    pbuf_t *pb = c->rx_spare;
    c->rx_spare = NULL;
    pb->pb_pktlen = 0;
    pb->pb_offset = 2;
    pb->pb_buflen = rxlen;
    pb->pb_flags = (b0 & BLE_LLID_MASK) == BLE_LLID_START ? PBUF_SOP : 0;
    memcpy(pbuf_data(pb, 0), payload, rxlen);
    c->rx_data++;
    l2cap_input(&c->l2c, pb);
    break;
  }
  }

refill:
  // Runs while our AutoTx response is on the air, not T_IFS-critical
  if(c->rx_spare == NULL)
    c->rx_spare = pbuf_make(0, 0);
  return 0;
}

// Wait for the AutoTx TxDone. ANY ClrIrqStatus during the AutoTx
// countdown cancels the pending transmission - selective bit masks
// included (tested: clearing only the RX bits kills it just as dead
// as 0xffff), so DIO1 cannot be lowered and reused as a signal.
//
// With a TX_DONE pin wired (chip DIO3, mapped there alone so nothing
// needs to clear it): sleep on its interrupt. Prompt detection matters
// beyond stats: the continuation re-arm must land within T_IFS of our
// TX end or the master's chained packet is lost. Without the pin:
// poll GetIrqStatus over SPI.
//
// Clears IRQs once done. Returns 1 if the transmission fired, 0 if
// not, negative on error.
static int
ble_conn_wait_txdone(ble_conn_t *c, sx1280_t *s, int64_t t_ev,
                     uint8_t tx_len)
{
  error_t err;
  int fired = 0;

  // AutoTx fires ~200µs after RX end; add the TX airtime and margin
  const int64_t tx_deadline = t_ev + 200 + BLE_LL_TIME(tx_len) + 500;

  if(s->dio_txdone != GPIO_UNUSED) {
    if(sx1280_wait_txdone_pin(s, tx_deadline)) {
      fired = 1;
    } else {
      // Distinguish an unfired AutoTx from a dead wire
      uint8_t ts[4] = {SX1280_GET_IRQSTATUS};
      if((err = sx1280_cmd(s, ts, ts, sizeof(ts))) != 0)
        return err;
      if((ts[2] << 8 | ts[3]) & SX1280_IRQ_TX_DONE) {
        fired = 1;
        if(c->txdone_pin_miss++ == 0)
          evlog(LOG_WARNING, "%s: TX_DONE set but DIO2 low; wire?",
                s->name);
      }
    }
  } else {
    while(clock_get() < tx_deadline) {
      uint8_t ts[4] = {SX1280_GET_IRQSTATUS};
      if((err = sx1280_cmd(s, ts, ts, sizeof(ts))) != 0)
        return err;
      if((ts[2] << 8 | ts[3]) & SX1280_IRQ_TX_DONE) {
        fired = 1;
        break;
      }
    }
  }

  if(fired)
    c->tx_fired++;
  else
    c->tx_nofire++;

  if((err = sx1280_irq_clear(s)) != 0)
    return err;
  return fired;
}

// Between exchanges within one event: back into RX quickly. AutoTx
// stays armed from the event setup (it survives its own firing) and
// the chip parks in FS (AutoFS), so no standby round-trip here.
static error_t
ble_conn_rearm(ble_conn_t *c, sx1280_t *s)
{
  error_t err;

  uint8_t fb[4] = {SX1280_WRITE_BUFFER, BLE_CONN_TX_BASE,
                   BLE_LLID_CONT, 0};
  if(!c->last_rx_sn)
    fb[2] |= BLE_NESN;
  if(c->tx_seq)
    fb[2] |= BLE_SN;
  if((err = sx1280_cmd(s, fb, NULL, sizeof(fb))) != 0)
    return err;

  static const uint8_t rx[4] = {SX1280_SET_RX, SX1280_TICK_SIZE_1_MS, 0, 3};
  return sx1280_cmd(s, rx, NULL, sizeof(rx));
}

// Ask the master for a shorter connection interval (7.5-15ms) via an
// L2CAP Connection Parameter Update Request; the master answers with
// LL_CONNECTION_UPDATE_IND which the event loop already applies.
static void
ble_conn_request_conn_params(ble_conn_t *c)
{
  pbuf_t *pb = pbuf_make(0, 0);
  if(pb == NULL)
    return;
  uint8_t *d = pbuf_append(pb, 16);
  d[0] = 12; d[1] = 0;   // L2CAP length
  d[2] = 5;  d[3] = 0;   // CID: LE signaling
  d[4] = 0x12;           // Connection Parameter Update Request
  d[5] = 1;              // identifier
  d[6] = 8;  d[7] = 0;   // command length
  d[8] = 6;  d[9] = 0;   // interval min: 7.5ms
  d[10] = 12; d[11] = 0; // interval max: 15ms
  d[12] = 0; d[13] = 0;  // latency
  d[14] = 100; d[15] = 0; // supervision timeout: 1s
  pb->pb_flags |= PBUF_SOP;
  ble_conn_l2cap_output(&c->l2c, pb);
}

static int64_t
ble_conn_execute(sx1280_slot_t *slot, sx1280_t *s, int64_t now)
{
  ble_conn_t *c = (ble_conn_t *)slot;
  error_t err;

  if(c->term_code)
    return ble_conn_drop(c, s, c->term_code, c->term_sent ? "local" : "peer");

  if(c->term_req && !c->term_sent) {
    uint8_t *rsp = ble_conn_enqueue_ctrl(c, LL_TERMINATE_IND, 1);
    if(rsp != NULL) {
      rsp[0] = 0x16; // Connection terminated by local host
      c->term_sent = 1;
      // Spec 5.1.6: the procedure is bounded by the supervision timeout
      c->term_deadline = now + c->timeout;
    }
  }
  if(c->term_sent && now > c->term_deadline)
    return ble_conn_drop(c, s, 0x16, "local, unacked");

  if(c->established) {
    if(now - c->last_rx > c->timeout)
      return ble_conn_drop(c, s, 0x08, "supervision timeout");
  } else if(c->event_counter != 0xffff && c->event_counter >= 5) {
    return ble_conn_drop(c, s, 0x3e, "no first packet");
  }

  c->phase = 1;
  if(sx1280_sched_set_mode(s, c)) {
    if((err = ble_radio_setup(s)) != 0)
      goto recover;
  }

  c->event_counter++;


  if(c->established && !c->param_req_sent && c->event_counter >= 8) {
    c->param_req_sent = 1;
    ble_conn_request_conn_params(c);
  }

  // Some masters never initiate the data length procedure even when
  // our feature bit advertises it; ask ourselves (BT spec 5.1.9,
  // either side may). A DLE-less peer answers LL_UNKNOWN_RSP, which
  // is ignored and leaves the default 27 in effect.
  if(c->established && !c->length_req_sent && c->event_counter >= 12) {
    uint8_t *req = ble_conn_enqueue_ctrl(c, LL_LENGTH_REQ, 8);
    if(req != NULL) {
      ble_conn_fill_length(req);
      c->length_req_sent = 1;
    }
  }

  if(c->pending_chmask_valid &&
     c->pending_chmask_instant == c->event_counter) {
    ble_conn_update_channels(c->chmap, c->pending_chmask);
    c->pending_chmask_valid = 0;
  }

  if(c->pending_update_valid &&
     c->pending_update_instant == c->event_counter) {
    const uint8_t *u = c->pending_update;
    // The new transmit window opens WinOffset + 1.25ms after the
    // point where this event's anchor would have been
    c->anchor += 1250 + (u[1] | (u[2] << 8)) * 1250;
    c->window = u[0] * 1250;
    c->interval = (u[3] | (u[4] << 8)) * 1250;
    c->timeout = (u[7] | (u[8] << 8)) * 10000;
    c->pending_update_valid = 0;
    evlog(LOG_NOTICE, "%s: BLE connection update: interval %dus",
          s->name, (int)c->interval);
  }

  // Channel selection algorithm #1
  const uint8_t unmapped =
    (c->last_unmapped_channel + c->hop_increment) % 37;
  c->last_unmapped_channel = unmapped;
  const uint8_t ch = c->chmap[unmapped];

  c->phase = 2;
  if((err = ble_conn_config_event(c, s, ch)) != 0)
    goto recover;

  // Window widening: 600ppm combined clock drift since last resync
  const uint32_t ww = (now - c->last_sync) * 6 / 10000 + 32;
  const int64_t anchor0 = c->anchor;
  const int64_t listen_end = c->anchor + c->window + ww;
  int got_packet = 0;

  while(!got_packet) {
    // The RX timeout keeps counting during the AutoTx delay and
    // cancels the pending transmission if it expires, so it must
    // cover an attended exchange: a full-size packet ending late in
    // the window plus the AutoTx fire point. Beyond that, keep it
    // close to our abandonment point so an unattended window does
    // not linger armed.
    const int rx_pad = BLE_LL_TIME(BLE_LL_DLE_PAYLOAD) + 500;
    int64_t dur = listen_end + rx_pad - clock_get();
    if(dur < rx_pad - 300)
      break;
    uint32_t ticks = dur * 64 / 1000 + 1;
    if(ticks > 0xffff)
      ticks = 0xffff;

    const uint8_t rx[4] = {SX1280_SET_RX, SX1280_TICK_SIZE_15_6_US,
                           ticks >> 8, ticks};
    if((err = sx1280_cmd(s, rx, NULL, sizeof(rx))) != 0)
      goto recover;

    // Sleep until the packet (or window end). RxDone fires at packet
    // END, so a packet starting just inside the window completes up
    // to one full DLE airtime later. The wakeup latency (~10-30µs)
    // fits comfortably inside the AutoTx response budget.
    if(!sx1280_wait_dio1(s, listen_end + BLE_LL_TIME(BLE_LL_DLE_PAYLOAD) + 200))
      break; // Window exhausted with radio timeout imminent

    const int64_t t_ev = clock_get();

    // Peek IRQ status without clearing: AutoTx may be counting down
    uint8_t st[4] = {SX1280_GET_IRQSTATUS};
    if((err = sx1280_cmd(s, st, st, sizeof(st))) != 0)
      goto recover;
    const uint16_t irq = st[2] << 8 | st[3];

    if(irq & SX1280_IRQ_RX_TX_TIMEOUT) {
      if((err = sx1280_irq_clear(s)) != 0)
        goto recover;
      break; // Missed event
    }

    if(irq & SX1280_IRQ_CRC_ERROR) {
      // Abort the armed AutoTx: never respond to a corrupt packet
      static const uint8_t sb[2] = {SX1280_SET_STANDBY, SX1280_STDBY_RC};
      if((err = sx1280_cmd(s, sb, NULL, sizeof(sb))) != 0)
        goto recover;
      if((err = sx1280_irq_clear(s)) != 0)
        goto recover;
      // SetStandby disarmed AutoTx; re-arm for the rest of the window
      static const uint8_t atx[3] = {SX1280_SET_AUTOTX, 0, BLE_AUTOTX_TIME};
      if((err = sx1280_cmd(s, atx, NULL, sizeof(atx))) != 0)
        goto recover;
      c->ev_crc++;
      continue;
    }

    if(!(irq & SX1280_IRQ_RX_DONE))
      continue;

    c->phase = 3;
    // T_IFS-critical path: read header, build response, write it
    uint8_t hb[3 + 2] = {SX1280_READ_BUFFER, 0, 0};
    if((err = sx1280_cmd(s, hb, hb, sizeof(hb))) != 0)
      goto recover;
    const uint8_t rx_b0 = hb[3];
    const uint8_t rxlen = hb[4];

    uint8_t tx_hdr, tx_len;
    ble_conn_pick_tx(c, rx_b0, rxlen, &tx_hdr, &tx_len);

    uint8_t wb[2 + 2 + BLE_LL_DLE_PAYLOAD];
    wb[0] = SX1280_WRITE_BUFFER;
    wb[1] = BLE_CONN_TX_BASE;
    wb[2] = tx_hdr;
    wb[3] = tx_len;
    memcpy(wb + 4, c->tx_pdu + 2, tx_len);
    if((err = sx1280_cmd(s, wb, NULL, 4 + tx_len)) != 0)
      goto recover;

    const uint32_t patch = clock_get() - t_ev;
    if(patch > c->max_patch)
      c->max_patch = patch;

    c->phase = 4;
    int fired = ble_conn_wait_txdone(c, s, t_ev, tx_len);
    if(fired < 0) {
      err = fired;
      goto recover;
    }

    // Anchor resync: the packet started one airtime before RxDone
    c->anchor = t_ev - (10 + rxlen) * 8 - 8;
    c->last_sync = c->anchor;
    c->last_rx = t_ev;
    c->established = 1;
    c->window = 0;
    c->ev_rx++;
    c->rx_pdus++;
    got_packet = 1;

    c->phase = 5;
    if((err = ble_conn_process_rx(c, s, rx_b0, rxlen)) != 0)
      goto recover;

    // --- More exchanges within this event while either side has
    // data (MD bit). One exchange per T_IFS pair.
    uint8_t prev_b0 = rx_b0;
    const int64_t budget = anchor0 + (int64_t)c->interval * 3 / 4;

    while(fired > 0 && clock_get() < budget &&
          ((prev_b0 & BLE_MD) || ble_conn_tx_pending(c))) {

      c->phase = 6;
      if((err = ble_conn_rearm(c, s)) != 0)
        goto recover;

      if(!sx1280_wait_dio1(s, clock_get() + 150 +
                           BLE_LL_TIME(BLE_LL_DLE_PAYLOAD) + 300))
        break; // Master closed the event

      const int64_t t_ex = clock_get();

      uint8_t st2[4] = {SX1280_GET_IRQSTATUS};
      if((err = sx1280_cmd(s, st2, st2, sizeof(st2))) != 0)
        goto recover;
      const uint16_t irq2 = st2[2] << 8 | st2[3];

      if(!(irq2 & SX1280_IRQ_RX_DONE) || (irq2 & SX1280_IRQ_CRC_ERROR)) {
        static const uint8_t sb2[2] = {SX1280_SET_STANDBY,
                                       SX1280_STDBY_RC};
        if((err = sx1280_cmd(s, sb2, NULL, sizeof(sb2))) != 0)
          goto recover;
        if((err = sx1280_irq_clear(s)) != 0)
          goto recover;
        break;
      }

      uint8_t hb2[3 + 2] = {SX1280_READ_BUFFER, 0, 0};
      if((err = sx1280_cmd(s, hb2, hb2, sizeof(hb2))) != 0)
        goto recover;
      const uint8_t b0 = hb2[3];
      const uint8_t len = hb2[4];

      uint8_t th, tl;
      ble_conn_pick_tx(c, b0, len, &th, &tl);
      wb[2] = th;
      wb[3] = tl;
      memcpy(wb + 4, c->tx_pdu + 2, tl);
      if((err = sx1280_cmd(s, wb, NULL, 4 + tl)) != 0)
        goto recover;

      fired = ble_conn_wait_txdone(c, s, t_ex, tl);
      if(fired < 0) {
        err = fired;
        goto recover;
      }

      c->last_rx = t_ex;
      c->rx_pdus++;

      if((err = ble_conn_process_rx(c, s, b0, len)) != 0)
        goto recover;

      prev_b0 = b0;
    }
  }

  // Deterministic end-of-event state. The chip may still be sitting
  // in RX with AutoTx armed (abandoned window, or the master closed
  // the event): a late packet then triggers a phantom auto-
  // transmission that collides with the next slot's commands and
  // wedges the chip, BUSY stuck high. SetStandby discards any armed
  // AutoTx (datasheet 13.2.4.1) and closes the window.
  c->phase = 7;
  static const uint8_t sb_end[2] = {SX1280_SET_STANDBY, SX1280_STDBY_RC};
  if((err = sx1280_cmd(s, sb_end, NULL, sizeof(sb_end))) != 0)
    goto recover;
  if((err = sx1280_irq_clear(s)) != 0)
    goto recover;

  if(!got_packet) {
    c->ev_missed++;
    if(c->coll_missed < 4)
      c->coll_missed++;
  } else {
    c->coll_missed = 0;
  }

  // The l2cap layer stops pulling from services at high water on our
  // TX queue (or when the pbuf pool ran dry, which our draining is
  // what remedies); once drained, ask it to refill. Racing the flag
  // against a concurrent set on the net thread is benign: that set
  // implies the queue was just at high water, and the next event
  // rechecks.
  if(c->l2c.l2c_tx_throttled &&
     c->l2c.l2c_tx_queue_len <= L2CAP_TXQ_LOW) {
    c->l2c.l2c_tx_throttled = 0;
    l2cap_txq_pump(&c->l2c);
  }

  // Fairness between overlapping connections: with equal priority and
  // equal intervals the same connection would lose every collision,
  // so missed events raise this slot's priority until it wins one.
  // A connection nearing its supervision timeout outranks everything.
  uint8_t prio = BLE_CONN_PRIO + (c->coll_missed > 1 ? c->coll_missed : 0);
  if(c->established &&
     now - c->last_rx > (int64_t)c->timeout / 2)
    prio = BLE_CONN_PRIO + 6;
  c->slot.ss_prio = prio;

  c->anchor += c->interval;
  return c->anchor - BLE_CONN_LEAD;

recover:
  // Commanding the chip while a late packet triggers its armed AutoTx
  // can wedge it (BUSY stuck); the race cannot be fully closed from
  // this side. A wedge is survivable: every event reconfigures the
  // radio from scratch, so reset the chip and treat this as one
  // missed event. Repeated recovers mean real trouble - drop then.
  evlog(LOG_WARNING, "%s: conn recover err:%s phase:%d busy:%d",
        s->name, error_to_string(err), c->phase,
        gpio_get_input(s->busy));
  sx1280_sched_recover(s);
  if(++c->recovers > 8)
    return ble_conn_drop(c, s, 0x3e, error_to_string(err));
  c->ev_missed++;
  if(c->coll_missed < 4)
    c->coll_missed++;
  c->anchor += c->interval;
  return c->anchor - BLE_CONN_LEAD;
}

// Accept a CONNECT_IND received at rx_end (LLData layout per BT spec
// 2.3.3.1): AA[4] CRCInit[3] WinSize[1] WinOffset[2] Interval[2]
// Latency[2] Timeout[2] ChM[5] Hop:5|SCA:3
static void
ble_conn_start(sx1280_t *s, const uint8_t *pdu, int64_t rx_end)
{
  ble_conn_t *c = NULL;
  for(int i = 0; i < BLE_MAX_CONN; i++) {
    if(g_conns[i] == NULL) {
      g_conns[i] = calloc(1, sizeof(ble_conn_t));
      g_conns[i]->chip = s;
      g_conns[i]->slot.ss_execute = ble_conn_execute;
      g_conns[i]->slot.ss_duration = 5000;
      g_conns[i]->slot.ss_prio = BLE_CONN_PRIO;
    }
    if(!g_conns[i]->active) {
      c = g_conns[i];
      break;
    }
  }
  if(c == NULL)
    return; // Pool full; the CONNECT_IND goes unanswered

  const uint8_t *ll = pdu + 14;

  c->access_addr = ll[0] | (ll[1] << 8) | (ll[2] << 16) | (ll[3] << 24);
  c->crc_init = ll[4] | (ll[5] << 8) | (ll[6] << 16);
  c->interval = (ll[10] | (ll[11] << 8)) * 1250;
  c->timeout = (ll[14] | (ll[15] << 8)) * 10000;
  ble_conn_update_channels(c->chmap, ll + 16);
  c->hop_increment = ll[21] & 0x1f;
  c->last_unmapped_channel = 0;

  c->anchor = rx_end + 1250 + (ll[8] | (ll[9] << 8)) * 1250;
  c->window = ll[7] * 1250;
  c->last_sync = rx_end;
  c->last_rx = rx_end;

  c->event_counter = 0xffff;
  c->last_rx_sn = BLE_SN; // Expect SN=0 first
  c->tx_seq = 0;
  c->tx_dummy = 1;
  c->tx_src = 0;
  c->tx_pdu[0] = BLE_LLID_CONT;
  c->tx_pdu[1] = 0;
  c->tx_frag_off = 0;
  c->eff_tx = BLE_LL_MAX_PAYLOAD;
  if(c->rx_spare == NULL)
    c->rx_spare = pbuf_make(0, 0);
  c->ctrlq_head = 0;
  c->ctrlq_count = 0;
  c->established = 0;
  c->term_code = 0;
  c->version_sent = 0;
  c->pending_chmask_valid = 0;
  c->pending_update_valid = 0;
  c->ev_rx = 0;
  c->ev_missed = 0;
  c->ev_crc = 0;
  c->tx_acked = 0;
  c->tx_retrans = 0;
  c->rx_bad_seq = 0;
  c->rx_data = 0;
  c->rx_drops = 0;
  c->rx_pdus = 0;
  c->coll_missed = 0;
  c->recovers = 0;
  c->slot.ss_prio = BLE_CONN_PRIO;
  c->param_req_sent = 0;
  c->length_req_sent = 0;
  c->term_req = 0;
  c->term_sent = 0;
  c->tx_fired = 0;
  c->tx_nofire = 0;
  c->txdone_pin_miss = 0;
  c->max_patch = 0;
  memcpy(c->peer_addr, pdu + 2, 6);
  c->autotx_time = g_ble_autotx_time;

  // Attach the mios BLE host stack
  c->l2c.l2c_output = ble_conn_l2cap_output;
  c->l2c.l2c_ltk_reply = NULL; // No link encryption (yet)
  STAILQ_INIT(&c->l2c.l2c_tx_queue);
  c->l2c.l2c_tx_queue_len = 0;
  memcpy(c->l2c.l2c_peer_addr, pdu + 2, 6);
  c->l2c.l2c_peer_addr_type = (pdu[0] & 0x40) ? 1 : 0; // TxAdd
  memcpy(c->l2c.l2c_our_addr, ble_our_addr, 6);
  c->l2c.l2c_our_addr_type = 1; // Static random

  if(l2cap_connect(&c->l2c) != 0)
    return; // Host stack refused; keep advertising

  c->active = 1;

  sx1280_sched_submit(s, &c->slot, c->anchor - BLE_CONN_LEAD);
}

static size_t
build_adv_pdu(uint8_t *p, const char *name)
{
  const size_t namelen = strlen(name);

  p[0] = 0x40; // ADV_IND (connectable undirected), TxAdd=1 (random address)
  p[1] = 6 + 3 + 2 + namelen;
  memcpy(p + 2, ble_our_addr, 6);
  // Flags: LE general discoverable, no BR/EDR
  p[8] = 2;
  p[9] = 0x01;
  p[10] = 0x06;
  // Complete local name
  p[11] = namelen + 1;
  p[12] = 0x09;
  memcpy(p + 13, name, namelen);
  return 13 + namelen;
}

// seed_override >= 0 forces the whitening seed (sweep mode), otherwise
// it is derived from the channel
static error_t
ble_adv_tx(sx1280_ble_adv_t *a, int ch, const char *name, int seed_override)
{
  sx1280_t *s = a->chip;
  error_t err;

  const uint32_t f = SX1280_HZ_TO_FREQ(adv_freq[ch]);
  const uint8_t freq[4] = {SX1280_SET_RFFREQUENCY, f >> 16, f >> 8, f};
  if((err = sx1280_cmd(s, freq, NULL, sizeof(freq))) != 0)
    return err;

  // The whitening LFSR loads bit-reversed relative to the BLE spec's
  // (and nRF hardware's) convention: ch37 wants 0x53, not 0x65.
  // Verified empirically with a seed sweep against a phone scanner.
  uint8_t seed = bitrev7(0x40 | adv_channels[ch]);
  if(seed_override >= 0)
    seed = seed_override;
  if((err = sx1280_write_reg(s, SX1280_REG_WHITENING_SEED, &seed, 1)) != 0)
    return err;

  uint8_t buf[2 + BLE_ADV_PDU_MAX];
  buf[0] = SX1280_WRITE_BUFFER;
  buf[1] = 0;
  size_t pdulen = build_adv_pdu(buf + 2, name);
  if((err = sx1280_cmd(s, buf, NULL, 2 + pdulen)) != 0)
    return err;

  if((err = sx1280_irq_clear(s)) != 0)
    return err;

  // 10ms timeout, packet is ~400µs
  static const uint8_t tx[4] = {SX1280_SET_TX, SX1280_TICK_SIZE_1_MS, 0, 10};
  if((err = sx1280_cmd(s, tx, NULL, sizeof(tx))) != 0)
    return err;

  // Sleep until TxDone. The T_IFS response window opens 150µs after
  // our TX ends and the wakeup costs ~1µs, so SetRx (the FIRST
  // command sent after waking) still lands with margin to spare.
  if(!sx1280_wait_dio1(s, clock_get() + 2000)) {
    a->tx_timeout++;
    // One lost TxDone is noise; a streak means the radio is wedged
    // and needs the full recover path (chip reset)
    if(++a->tmo_streak >= 3)
      return ERR_TIMEOUT;
    return sx1280_irq_ack(s) < 0 ? ERR_TIMEOUT : 0;
  }
  const uint64_t t0 = clock_get();

  // Ack TxDone first: SetRx issued during the chip's own TxDone->FS
  // (AutoFS) transition is silently lost
  int irq = sx1280_irq_ack(s);
  if(irq < 0)
    return irq;
  if(irq & SX1280_IRQ_TX_DONE) {
    a->tx_done++;
    a->tmo_streak = 0;
  }

  // Listen for SCAN_REQ / CONNECT_IND addressed to us: T_IFS (150µs)
  // + CONNECT_IND airtime (~352µs) + margin
  static const uint8_t rx[4] = {SX1280_SET_RX, SX1280_TICK_SIZE_1_MS, 0, 1};
  if((err = sx1280_cmd(s, rx, NULL, sizeof(rx))) != 0)
    return err;

  const uint32_t turnaround = clock_get() - t0;
  if(turnaround > a->max_turnaround)
    a->max_turnaround = turnaround;

  irq = sx1280_wait_irq(s, 1500);
  if(irq < 0)
    return irq;

  const uint32_t rx_latency = clock_get() - t0;
  if(irq == 0 || (irq & SX1280_IRQ_RX_TX_TIMEOUT))
    return 0; // Nobody talked to us

  if(irq & SX1280_IRQ_CRC_ERROR) {
    a->rx_crc_errors++;
    return 0;
  }

  if(!(irq & SX1280_IRQ_RX_DONE))
    return 0;

  // GetRxBufferStatus reports the PDU payload length; the buffer
  // additionally holds the 2-byte header in front of it
  uint8_t bs[4] = {SX1280_GET_RXBUFFERSTATUS};
  if((err = sx1280_cmd(s, bs, bs, sizeof(bs))) != 0)
    return err;
  const uint8_t payloadlen = bs[2];
  const uint8_t offset = bs[3];

  if(payloadlen < 6 || payloadlen + 2 > BLE_ADV_PDU_MAX)
    return 0;

  uint8_t rbuf[3 + BLE_ADV_PDU_MAX];
  rbuf[0] = SX1280_READ_BUFFER;
  rbuf[1] = offset;
  rbuf[2] = 0;
  if((err = sx1280_cmd(s, rbuf, rbuf, 3 + 2 + payloadlen)) != 0)
    return err;

  const uint8_t *pdu = rbuf + 3;
  const uint8_t pdu_type = pdu[0] & 0xf;

  // SCAN_REQ: ScanA(6) + AdvA(6). CONNECT_IND: InitA(6) + AdvA(6) +
  // LLData(22). Both carry our address at offset 8.
  if(pdu_type == 0x3 && payloadlen >= 12 &&
     !memcmp(pdu + 8, ble_our_addr, 6)) {
    a->rx_scan_req++;
  } else if(pdu_type == 0x5 && payloadlen >= 34 &&
            !memcmp(pdu + 8, ble_our_addr, 6)) {
    a->rx_conn_ind++;
    const uint8_t *ll = pdu + 14;
    evlog(LOG_NOTICE, "%s: CONNECT_IND from %02x:%02x:%02x:%02x:%02x:%02x "
          "AA:%02x%02x%02x%02x interval:%d.%02dms timeout:%dms hop:%d",
          s->name,
          pdu[7], pdu[6], pdu[5], pdu[4], pdu[3], pdu[2],
          ll[3], ll[2], ll[1], ll[0],
          (ll[10] | ll[11] << 8) * 125 / 100,
          (ll[10] | ll[11] << 8) * 125 % 100,
          (ll[14] | ll[15] << 8) * 10,
          ll[21] & 0x1f);

    // rx_latency was measured to DIO1 detection; the IRQ ack inside
    // the wait costs ~25µs on top of the actual packet end
    ble_conn_start(s, pdu, t0 + rx_latency - 25);
  } else if((pdu_type == 0x3 || pdu_type == 0x5) && payloadlen >= 12) {
    // Directed PDU that failed our address check: log the first one
    a->rx_misaddr++;
    if(a->rx_misaddr == 1)
      evlog(LOG_NOTICE, "%s: type:%d for %02x:%02x:%02x:%02x:%02x:%02x "
            "(not us)", s->name, pdu_type,
            pdu[13], pdu[12], pdu[11], pdu[10], pdu[9], pdu[8]);
  } else {
    a->rx_other++;
  }
  return 0;
}

// One advertising channel per slot invocation, so the slot stays
// short (~2ms worst case) and the arbiter can fit it between
// connection events. Advertising continues while connected; new
// CONNECT_INDs go to free pool entries.
static int64_t
ble_adv_execute(sx1280_slot_t *slot, sx1280_t *s, int64_t now)
{
  sx1280_ble_adv_t *a = (sx1280_ble_adv_t *)slot;
  error_t err = 0;

  if(sx1280_sched_set_mode(s, ble_mode)) {
    err = ble_radio_setup(s);
    if(err) {
      a->cmd_errors++;
      sx1280_sched_recover(s);
      return now + 1000000;
    }
  }

  if(a->sweep) {
    // Channel 37 only: the correct seed is channel-dependent, so a
    // fixed candidate can only match one channel. The name carries
    // the candidate so a scanner shows which one decodes.
    char name[24];
    snprintf(name, sizeof(name), "mios-%02x", a->sweep_seed);
    err = ble_adv_tx(a, 0, name, a->sweep_seed);
    a->sweep_cnt++;
    if(a->sweep_cnt >= 3) { // ~300ms per candidate
      a->sweep_cnt = 0;
      a->sweep_seed = (a->sweep_seed + 1) & 0x7f;
    }
  } else {
    err = ble_adv_tx(a, a->ch, a->name, -1);
  }

  if(err) {
    a->cmd_errors++;
    sx1280_sched_recover(s);
    return now + 1000000;
  }

  if(!a->sweep) {
    a->ch++;
    if(a->ch < 3)
      return now; // Remaining channels of this adv event, ASAP
    a->ch = 0;
  }

  // advInterval + advDelay jitter
  return now + BLE_ADV_INTERVAL + (clock_get() & 0x1fff);
}

static sx1280_ble_adv_t *
sx1280_ble_adv_get(sx1280_t *s)
{
  ble_addr_init();
  if(g_adv == NULL) {
    g_adv = calloc(1, sizeof(sx1280_ble_adv_t));
    g_adv->chip = s;
    g_adv->slot.ss_execute = ble_adv_execute;
    g_adv->slot.ss_duration = 2500; // One channel per slot
    g_adv->slot.ss_prio = 1;
    strcpy(g_adv->name, "mios");
  }
  return g_adv;
}

void
sx1280_ble_adv_start(sx1280_t *s, const char *name, int sweep)
{
  sx1280_ble_adv_t *a = sx1280_ble_adv_get(s);
  if(name != NULL)
    strlcpy(a->name, name, sizeof(a->name));
  a->sweep = sweep;
  sx1280_sched_submit(s, &a->slot, clock_get());
}

void
sx1280_ble_adv_stop(sx1280_t *s)
{
  if(g_adv != NULL)
    sx1280_sched_cancel(s, &g_adv->slot);
}

int
sx1280_ble_conn_active(sx1280_t *s)
{
  for(int i = 0; i < BLE_MAX_CONN; i++)
    if(g_conns[i] != NULL && g_conns[i]->active)
      return 1;
  return 0;
}

void
sx1280_ble_conn_drop(sx1280_t *s)
{
  for(int i = 0; i < BLE_MAX_CONN; i++)
    if(g_conns[i] != NULL && g_conns[i]->active)
      g_conns[i]->term_req = 1; // Picked up by the radio thread
}

void
sx1280_ble_set_dle(sx1280_t *s, int octets)
{
  if(octets < 27)
    octets = 27;
  if(octets > BLE_LL_DLE_PAYLOAD)
    octets = BLE_LL_DLE_PAYLOAD;
  g_ble_dle_max = octets;
}

void
sx1280_ble_set_autotx(sx1280_t *s, int val)
{
  g_ble_autotx_time = val;
  for(int i = 0; i < BLE_MAX_CONN; i++)
    if(g_conns[i] != NULL)
      g_conns[i]->autotx_time = val;
}

void
sx1280_ble_set_txpower(sx1280_t *s, int dbm)
{
  if(dbm < -18)
    dbm = -18;
  if(dbm > 13)
    dbm = 13;
  g_ble_tx_dbm = dbm;
  sx1280_sched_set_mode(s, NULL); // Force reconfig
}

void
sx1280_ble_adv_report(sx1280_t *s, struct stream *st)
{
  const sx1280_ble_adv_t *a = sx1280_ble_adv_get(s);
  stprintf(st, "tx_done:%d tx_timeout:%d errors:%d\n",
           (int)a->tx_done, (int)a->tx_timeout, (int)a->cmd_errors);
  stprintf(st, "addr:%02x:%02x:%02x:%02x:%02x:%02x name:%s\n",
           ble_our_addr[5], ble_our_addr[4], ble_our_addr[3],
           ble_our_addr[2], ble_our_addr[1], ble_our_addr[0], a->name);
  stprintf(st, "scan_req:%d conn_ind:%d misaddr:%d other:%d crc_errors:%d "
           "max_turnaround:%dus\n",
           (int)a->rx_scan_req, (int)a->rx_conn_ind, (int)a->rx_misaddr,
           (int)a->rx_other, (int)a->rx_crc_errors,
           (int)a->max_turnaround);
}

// Stack hook for the shared ble_connections command; per-connection
// LL state lives here, radio-level counters in the device print
void
ble_print_connections(struct stream *st)
{
  int n = 0;
  for(int i = 0; i < BLE_MAX_CONN; i++) {
    ble_conn_t *c = g_conns[i];
    if(c == NULL || !c->active)
      continue;
    n++;
    ble_print_connection_header(st, i, c->peer_addr,
                                c->established ? "UP" : "establishing",
                                c->l2c.l2c_sec_level,
                                c->interval, c->timeout);
    stprintf(st, "  ll: ev_rx:%d missed:%d crc:%d pdus:%d acked:%d "
             "retrans:%d bad_seq:%d data:%d nak:%d etx:%d patch:%dus\n",
             (int)c->ev_rx, (int)c->ev_missed, (int)c->ev_crc,
             (int)c->rx_pdus, (int)c->tx_acked, (int)c->tx_retrans,
             (int)c->rx_bad_seq, (int)c->rx_data, (int)c->rx_drops,
             (int)c->eff_tx, (int)c->max_patch);
    l2cap_print(&c->l2c, st);
  }
  if(n == 0)
    stprintf(st, "No connections\n");
}

