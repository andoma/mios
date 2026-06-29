// BLE peripheral link-layer for the nRF54L radio. Ported from the nRF52 one
// (src/platform/nrf52/nrf52_radio.c): the link-layer / l2cap / netif logic is
// kept verbatim; what changed is the hardware layer:
//   - the modern nRF54L RADIO register map (events 0x200+, config 0xE00+),
//   - TIMER10 (radio-power-domain timer) instead of TIMER0,
//   - DPPIC10 (publish/subscribe) instead of the nRF52 fixed PPI channels,
//   - HFXO kept running while the stack is up (no per-event xtal management),
//   - INTENSET00 / RADIO_0 + TIMER10 NVIC lines, FICR device address.

#include "nrf54l_reg.h"
#include "nrf54l_radio.h"

#include "net/pbuf.h"
#include "net/ble/l2cap.h"
#include "net/ble/ble_proto.h"

#include <sys/param.h>

#include <mios/timer.h>
#include <mios/task.h>
#include <mios/ghook.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>

#include "irq.h"

// --- HFXO ------------------------------------------------------------------
#define CLOCK_BASE             0x5010e000
#define CLOCK_TASKS_XOSTART    (CLOCK_BASE + 0x000)
#define CLOCK_EVENTS_XOSTARTED (CLOCK_BASE + 0x100)

// --- RADIO (modern Nordic radio IP) ----------------------------------------
#define RADIO_BASE 0x5008a000

#define RADIO_TASKS_TXEN      (RADIO_BASE + 0x000)
#define RADIO_TASKS_RXEN      (RADIO_BASE + 0x004)
#define RADIO_TASKS_DISABLE   (RADIO_BASE + 0x010)
#define RADIO_SUBSCRIBE_TXEN  (RADIO_BASE + 0x100)
#define RADIO_EVENTS_ADDRESS  (RADIO_BASE + 0x20c)
#define RADIO_EVENTS_END      (RADIO_BASE + 0x218)
#define RADIO_EVENTS_DISABLED (RADIO_BASE + 0x220)
#define RADIO_EVENTS_CRCOK    (RADIO_BASE + 0x22c)
#define RADIO_PUBLISH_ADDRESS (RADIO_BASE + 0x30c)
#define RADIO_PUBLISH_END     (RADIO_BASE + 0x318)
#define RADIO_SHORTS          (RADIO_BASE + 0x400)
#define RADIO_INTENSET00      (RADIO_BASE + 0x488)
#define RADIO_MODE            (RADIO_BASE + 0x500)
#define RADIO_DATAWHITE       (RADIO_BASE + 0x540)
#define RADIO_FREQUENCY       (RADIO_BASE + 0x708)
#define RADIO_TXPOWER         (RADIO_BASE + 0x710)
#define RADIO_RSSISAMPLE      (RADIO_BASE + 0x718)
#define RADIO_CRCSTATUS       (RADIO_BASE + 0xe0c)
#define RADIO_PCNF0           (RADIO_BASE + 0xe20)
#define RADIO_PCNF1           (RADIO_BASE + 0xe28)
#define RADIO_BASE0           (RADIO_BASE + 0xe2c)
#define RADIO_PREFIX0         (RADIO_BASE + 0xe34)
#define RADIO_TXADDRESS       (RADIO_BASE + 0xe3c)
#define RADIO_RXADDRESSES     (RADIO_BASE + 0xe40)
#define RADIO_CRCCNF          (RADIO_BASE + 0xe44)
#define RADIO_CRCPOLY         (RADIO_BASE + 0xe48)
#define RADIO_CRCINIT         (RADIO_BASE + 0xe4c)
#define RADIO_PACKETPTR       (RADIO_BASE + 0xed0)

#define RADIO_IRQ             138 // RADIO_0

#define RADIO_SHORT_READY_START   (1 << 0)
#define RADIO_SHORT_PHYEND_DISABLE (1 << 19) // the new radio has no END_DISABLE
#define RADIO_INT_END             (1 << 6)   // INTENSET00.END

// --- TIMER10 (same register layout as the nRF52 TIMER) ---------------------
#define TIMER_BASE 0x50085000
#define TIMER_IRQ  133

#define TIMER_TASKS_START        0x000
#define TIMER_TASKS_STOP         0x004
#define TIMER_TASKS_CLEAR        0x00c
#define TIMER_SUBSCRIBE_CAPTURE(x) (0x0c0 + (x) * 4)
#define TIMER_EVENTS_COMPARE(x)  (0x140 + (x) * 4)
#define TIMER_PUBLISH_COMPARE(x) (0x1c0 + (x) * 4)
#define TIMER_INTENSET           0x304
#define TIMER_BITMODE            0x508
#define TIMER_PRESCALER          0x510
#define TIMER_CC(x)             (0x540 + (x) * 4)

// TIMER10 PCLK is 32 MHz; /32 -> 1 MHz, so the link-layer keeps working in µs.
#define TIMER_PRESCALER_1MHZ     5

// --- DPPIC10 (shared by RADIO and TIMER10, same power domain) ---------------
#define DPPIC_BASE 0x50082000
#define DPPIC_CHENSET (DPPIC_BASE + 0x504)
#define DPPIC_CHENCLR (DPPIC_BASE + 0x508)

#define DPPI_EN 0x80000000u // PUBLISH/SUBSCRIBE enable bit

// DPPI channel assignment (mirrors the nRF52 fixed PPI channels it replaces):
#define DPPI_CH_ADDRESS  0 // RADIO ADDRESS  -> TIMER CAPTURE[1]  (was PPI 26)
#define DPPI_CH_END      1 // RADIO END      -> TIMER CAPTURE[2]  (was PPI 27)
#define DPPI_CH_TXEN     2 // TIMER COMPARE0 -> RADIO TXEN        (was PPI 20)

// FICR factory device address.
#define FICR_DEVICEADDR0 0x00ffc3a4
#define FICR_DEVICEADDR1 0x00ffc3a8


// Link Layer State
typedef enum {
  LL_IDLE,
  LL_ADV_TX,
  LL_ADV_RX,
  LL_CONNECTED_IDLE,
  LL_CONNECTED_RX_1,
  LL_CONNECTED_RX_N,
  LL_CONNECTED_TX,
} ll_state_t;

const char state2str[8][8] = {
  "Idle",
  "Adv_TX",
  "Adv_RX",
  "ConnIdl",
  "ConnRx1",
  "ConnRxN",
  "ConnTx"
};


typedef struct connection {

  l2cap_t l2c;

  uint32_t access_addr;
  uint32_t crc_iv;

  // Time related variables are stored as µS

  uint32_t transmitWindowDelay;  // Depends on connect PDU
  uint32_t transmitWindowSize;   // from lldata
  uint32_t transmitWindowOffset; // from lldata
  uint32_t connInterval;
  uint32_t latency;
  uint32_t timeout;

  uint32_t next_anchor_point;
  uint32_t window_open_offset;
  uint32_t next_timeout;

  uint8_t hop_increment;
  uint8_t last_unmapped_channel;
  uint8_t chmap[37];

  uint8_t last_rx_seq;
  uint8_t tx_seq;
  uint8_t tx_dummy;
  uint8_t termination_code;

  uint8_t pending_chmask_valid;
  uint8_t pending_chmask[5];
  uint16_t pending_chmask_instant;

  uint16_t eventCounter;

  struct {
    uint32_t rx;
    uint32_t tx;
    uint32_t rx_qdrops;
    uint32_t rx_bad_seq;
    uint32_t rx_silent;
    uint32_t rx_crc;
    uint32_t tx_retransmissions;
    int rx_maxlen;
  } stat;

  uint8_t addr[6];

  uint8_t rssi;

} connection_t;


typedef struct nrf54l_radio {

  netif_t nr_bn;

  ll_state_t nr_ll_state;

  pbuf_t *nr_pbuf;

  timer_t nr_slow_timer;
  uint32_t nr_softirq_enter_adv;

  connection_t nr_con;

  uint32_t nr_announce_interval;

  uint8_t nr_addr[6];

  uint8_t nr_adv_pkt[2 + 6 + 31];

  uint8_t nr_empty_packet[2];

  uint8_t nr_more_data;

  uint8_t nr_adv_ch;
} nrf54l_radio_t;


static nrf54l_radio_t g_radio;

static void
build_adv_pkt(nrf54l_radio_t *nr, const char *name)
{
  const size_t namelen = strlen(name);

  uint8_t *p = nr->nr_adv_pkt;
  p[0] = ADV_IND | ADV_TXADD; // Random address
  p[1] = 6 + namelen + 2;
  memcpy(p + 2, nr->nr_addr, 6);
  p[8] = namelen + 1;
  p[9] = 9; // Complete Local Name
  memcpy(p + 10, name, namelen);
}


static const uint8_t channel_to_freq[40] = { // 2400 MHz + indexed value
     4,  6,  8, 10, 12, 14, 16, 18, 20, 22,  //  0 -  9
    24, 28, 30, 32, 34, 36, 38, 40, 42, 44,  // 10 - 19
    46, 48, 50, 52, 54, 56, 58, 60, 62, 64,  // 20 - 29
    66, 68, 70, 72, 74, 76, 78,              // 30 - 36
    2, 26, 80                                // 37 - 39  (ADV channels)
};


static void
select_channel(int channel, uint32_t crc_init, uint32_t access_addr)
{
  reg_wr(RADIO_FREQUENCY, channel_to_freq[channel]);
  // Whitening: keep the BLE polynomial (DATAWHITE.POLY reset = 0x89) and load
  // the per-channel IV = channel with bit 6 preset (the nRF54L radio does not
  // auto-force that bit, unlike the nRF52 one).
  reg_wr(RADIO_DATAWHITE, (0x89 << 16) | (0x40 | channel));
  reg_wr(RADIO_CRCINIT, crc_init);
  reg_wr(RADIO_BASE0, access_addr << 8);
  reg_wr(RADIO_PREFIX0, access_addr >> 24);
}


static void
radio_init_ble(void)
{
  reg_wr(RADIO_MODE, 3); // 1Mbit/s BLE
  reg_wr(RADIO_CRCPOLY, 0x65b);
  reg_wr(RADIO_CRCCNF,
         (1 << 8) | // Skip Address in CRC
         (3 << 0)); // 3 byte CRC

  reg_wr(RADIO_RXADDRESSES, 0x1); // logical address 0
  reg_wr(RADIO_TXADDRESS, 0);

  const int lflen = 8;
  const int s0len = 1;
  const int s1len = 0;

  reg_wr(RADIO_PCNF0,
         (lflen << 0)  | // Length of LEN in bits
         (s0len << 8)  | // Length of S0 in bytes
         (s1len << 16) | // Length of S1 in bytes
         0);

  const int maxlen = LLMTU;
  const int statlen = 0;
  reg_wr(RADIO_PCNF1,
         (maxlen << 0) |
         (statlen << 8) |
         (3 << 16) | // balen = 4 (3 + 1)
         (1 << 25)); // Enable whitening

  reg_wr(RADIO_INTENSET00, RADIO_INT_END);

  reg_wr(RADIO_SHORTS,
         RADIO_SHORT_READY_START |
         RADIO_SHORT_PHYEND_DISABLE);

  reg_wr(RADIO_TXPOWER, 0x18); // 0 dBm
}


static void
radio_setup_for_adv(nrf54l_radio_t *nr)
{
  nr->nr_adv_ch++;
  if(nr->nr_adv_ch == 3)
    nr->nr_adv_ch = 0;

  select_channel(37 + nr->nr_adv_ch, 0x555555, 0x8e89bed6);
}

static void
conn_update_channels(uint8_t *chmap, const uint8_t *mask)
{
  int num_used = 0;
  for(int i = 0; i <= 36; i++) {
    int used = mask[i / 8] & (1 << (i & 7));
    if(used) {
      chmap[num_used] = i;
      num_used++;
    }
  }

  for(int i = 36; i >= 0; i--) {
    int used = mask[i / 8] & (1 << (i & 7));
    if(used) {
      chmap[i] = i;
    } else {
      chmap[i] = chmap[i % num_used];
    }
  }
}


static void
conn_fini(connection_t *con)
{
  pbuf_free_queue_irq_blocked(&con->l2c.l2c_tx_queue);
  con->l2c.l2c_tx_queue_len = 0;
}


static void
conn_output(struct l2cap *self, struct pbuf *pb)
{
  connection_t *con = (connection_t *)self;

  if(pb == NULL) {
    // l2cap layer closed
    con->l2c.l2c_output = NULL;
    return;
  }

  int payload_len = pb->pb_pktlen;
  pb = pbuf_prepend(pb, 2, 1, 0);
  if(pbuf_pullup(pb, pb->pb_pktlen)) {
    panic("pullup failed");
  }

  uint8_t *hdr = pbuf_data(pb, 0);
  hdr[0] = pb->pb_flags & PBUF_SOP ? 2 : 1;
  hdr[1] = payload_len;

  int q = irq_forbid(IRQ_LEVEL_NET);
  STAILQ_INSERT_TAIL(&con->l2c.l2c_tx_queue, pb, pb_link);
  con->l2c.l2c_tx_queue_len++;
  irq_permit(q);
}

static void
conn_init(connection_t *con, const struct lldata *lld)
{
  con->l2c.l2c_output = conn_output;

  STAILQ_INIT(&con->l2c.l2c_tx_queue);

  conn_update_channels(con->chmap, lld->channel_mask);
  con->hop_increment = lld->hop_sca & 0x1f;
  con->last_unmapped_channel = 0;

  con->crc_iv =
    lld->crcinit[0] | (lld->crcinit[1] << 8) | (lld->crcinit[2] << 16);

  con->access_addr = lld->access_addr;

  con->transmitWindowDelay = 1250;
  con->transmitWindowSize = lld->win_size * 1250;
  con->transmitWindowOffset = lld->win_offset * 1250;
  con->connInterval = lld->interval * 1250;
  con->timeout = lld->timeout * 10000;

  // We say previously receved SN was 1 (thus, we expect 0)
  con->last_rx_seq = DATA_SN;
  con->tx_seq = 0;
  con->tx_dummy = 1;
  con->termination_code = 0;

  con->pending_chmask_valid = 0;

  con->eventCounter = 0xffff;
}


static void
conn_disconnect(nrf54l_radio_t *nr, uint32_t now)
{
  softirq_raise(nr->nr_softirq_enter_adv);
  ghook_invoke(GHOOK_BLE_STATUS, 0);

  nr->nr_ll_state = LL_IDLE;
  reg_wr(TIMER_BASE + TIMER_TASKS_STOP, 1);
}


static void
conn_hop(nrf54l_radio_t *nr, connection_t *con)
{
  // 4.5.8.2 Channel Selection algorithm #1
  uint8_t ch = (con->last_unmapped_channel + con->hop_increment) % 37;
  con->last_unmapped_channel = ch;
  select_channel(con->chmap[ch], con->crc_iv, con->access_addr);
}


static void
arm_timer3(uint32_t when)
{
  reg_wr(TIMER_BASE + TIMER_CC(3), when);
}


static int
handle_CONNECT_IND(nrf54l_radio_t *nr, const uint8_t *pkt)
{
  const struct lldata *lld = (struct lldata *)(pkt + 14);
  connection_t *con = &nr->nr_con;

  if(l2cap_connect(&con->l2c))
    return 0;

  memcpy(con->addr, pkt + 2, 6);

  conn_init(con, lld);

  uint32_t rx_end_time = reg_rd(TIMER_BASE + TIMER_CC(2));

  con->next_anchor_point =
    rx_end_time + con->transmitWindowDelay +
    con->transmitWindowOffset;

  con->window_open_offset = 1000;

  arm_timer3(con->next_anchor_point - con->window_open_offset);

  nr->nr_ll_state = LL_CONNECTED_IDLE;

  con->next_timeout = con->next_anchor_point  + con->timeout;
  netlog("ble: Connected to %02x:%02x:%02x:%02x:%02x:%02x RSSI:%d hop:%d (on %d)",
         pkt[2 + 5], pkt[2 + 4], pkt[2 + 3],
         pkt[2 + 2], pkt[2 + 1], pkt[2 + 0],
         -reg_rd(RADIO_RSSISAMPLE),
         con->hop_increment,
         37 + nr->nr_adv_ch);

  memset(&con->stat, 0, sizeof(con->stat));

  ghook_invoke(GHOOK_BLE_STATUS, 1);
  return 1;
}


static int
adv_rx(nrf54l_radio_t *nr)
{
  const uint8_t *pkt = pbuf_cdata(nr->nr_pbuf, 0);
  const uint8_t pdu_type = pkt[0] & 0xf;

  // We just listen for CONNECT
  if(pdu_type != CONNECT_IND)
    return 0;

  if(!(pkt[0] & ADV_RXADD))
    return 0;

  if(memcmp(pkt + 8, nr->nr_addr, 6))
    return 0; // Not for us

  return handle_CONNECT_IND(nr, pkt);
}


static uint8_t *
enqueue_ctrl_pdu(connection_t *con, uint8_t type, uint8_t len)
{
  pbuf_t *pb = pbuf_make_irq_blocked(0, 0);
  if(pb == NULL)
    return NULL; // No buffer -> no ack -> peer will retransmit later

  STAILQ_INSERT_TAIL(&con->l2c.l2c_tx_queue, pb, pb_link);
  con->l2c.l2c_tx_queue_len++;

  uint8_t *rsp = pbuf_data(pb, 0);
  rsp[0] = 3;
  rsp[1] = len + 1;
  rsp[2] = type;
  return rsp + 3;
}


static int
handle_ll_feature_req(connection_t *con, const uint8_t *req, int reqlen)
{
  uint8_t *rsp = enqueue_ctrl_pdu(con, LL_FEATURE_RSP, 8);
  if(rsp == NULL)
    return 0;
  memset(rsp, 0, 8);

  if(req[0] & (1 << 5)) {
    rsp[0] |= 1 << 5;
  }
  return 1;
}


static int
handle_unknown_ctrlop(connection_t *con, uint8_t op)
{
  uint8_t *rsp = enqueue_ctrl_pdu(con, LL_UNKNOWN_RSP, 1);
  if(rsp == NULL)
    return 0;
  rsp[0] = op;
  return 1;
}


static int
handle_ll_channel_map_ind(connection_t *con, const uint8_t *req, int reqlen)
{
  if(reqlen < 7)
    return 1;

  memcpy(&con->pending_chmask, req, 5);
  memcpy(&con->pending_chmask_instant, req + 5, 2);
  con->pending_chmask_valid = 1;
  return 1;
}


static int
handle_ll_version_ind(connection_t *con, const uint8_t *req, int reqlen)
{
  uint8_t *rsp = enqueue_ctrl_pdu(con, LL_VERSION_IND, 5);
  if(rsp == NULL)
    return 0;

  rsp[0] = 0x0c; // Core_v5.3
  rsp[1] = 0x59; // Nordic semiconductor
  rsp[2] = 0x00;
  rsp[3] = 0x00;
  rsp[4] = 0x00;
  return 1;
}

static int
handle_ll_length_req(connection_t *con, const uint8_t *req, int reqlen)
{
  uint8_t *rsp = enqueue_ctrl_pdu(con, LL_LENGTH_RSP, 8);
  if(rsp == NULL)
    return 0;

  int o = LLMTU;
  int t = 2120;
  rsp[0] = o;
  rsp[1] = o >> 8;
  rsp[2] = t;
  rsp[3] = t >> 8;
  rsp[4] = o;
  rsp[5] = o >> 8;
  rsp[6] = t;
  rsp[7] = t >> 8;
  return 1;
}


static int
conn_terminate(nrf54l_radio_t *nr, connection_t *con, uint8_t code,
               const char *origin)
{
  if(con->termination_code)
    return 1;

  netlog("ble: Terminated (code=0x%x) (%s)", code, origin);

  l2cap_disconnect(&con->l2c);

  con->termination_code = code;
  return 1;
}


static int
handle_ctrl_pdu(nrf54l_radio_t *nr, connection_t *con,
                const uint8_t *req, int reqlen)
{
  if(reqlen == 0)
    return 1; // Invalid according to spec, but we ack anyway

  switch(req[0]) {
  case LL_FEATURE_REQ:
    return handle_ll_feature_req(con, req + 1, reqlen - 1);
  case LL_CHANNEL_MAP_IND:
    return handle_ll_channel_map_ind(con, req + 1, reqlen - 1);
  case LL_VERSION_IND:
    return handle_ll_version_ind(con, req + 1, reqlen - 1);
  case LL_TERMINATE_IND:
    return conn_terminate(nr, con, req[1], "remote");
  case LL_LENGTH_REQ:
    return handle_ll_length_req(con, req + 1, reqlen - 1);
  default:
    return handle_unknown_ctrlop(con, req[0]);
  }
}


static void
conn_rx_done(nrf54l_radio_t *nr)
{
  connection_t *con = &nr->nr_con;
  int n = 0;

  nr->nr_more_data = 0;

  con->rssi = reg_rd(RADIO_RSSISAMPLE);

  if(reg_rd(RADIO_CRCSTATUS)) {

    if(nr->nr_ll_state == LL_CONNECTED_RX_1) {
      const uint32_t capt = reg_rd(TIMER_BASE + TIMER_CC(1));
      n = capt - (con->next_anchor_point - con->connInterval);
      con->next_anchor_point = capt + con->connInterval;
      arm_timer3(con->next_anchor_point - con->window_open_offset);
    }

    uint8_t *pkt = pbuf_data(nr->nr_pbuf, 0);

    const uint8_t b0 = pkt[0];
    const uint8_t len = pkt[1];
    con->stat.rx_maxlen = MAX(con->stat.rx_maxlen, len);

    if(b0 & DATA_SN)
      con->window_open_offset = 250;

    if(con->tx_seq == !(b0 & DATA_NESN)) {
      // Remote received our packet
      if(!con->tx_dummy) {
        pbuf_t *pb = pbuf_splice(&con->l2c.l2c_tx_queue);
        assert(pb != NULL);
        assert(pb->pb_next == NULL);
        assert(con->l2c.l2c_tx_queue_len > 0);
        pbuf_free_irq_blocked(pb);
        con->l2c.l2c_tx_queue_len--;
      }

      con->tx_seq = !con->tx_seq;
      con->stat.tx++;
    } else {
      con->stat.tx_retransmissions++;
    }

    if(b0 & DATA_MD)
      nr->nr_more_data |= 1;

    if((con->last_rx_seq ^ b0) & DATA_SN) {

      const uint8_t llid = b0 & 3;
      int ack;
      pbuf_t *pb;

      switch(llid) {
      case 0:  // Reserved future use
        ack = 1;
        break;
      case 1:
      case 2:
        if(!len || con->termination_code) {
          ack = 1;
        } else {
          pb = pbuf_make_irq_blocked(0, 0);
          if(pb == NULL) {
            ack = 0;
            break;
          }

          pbuf_t *rx = nr->nr_pbuf;
          rx->pb_pktlen = 0;
          rx->pb_buflen = len;
          rx->pb_offset = 2;
          rx->pb_flags = llid == 2 ? PBUF_SOP : 0;
          ack = 1;
          l2cap_input(&con->l2c, rx);
          nr->nr_pbuf = pb;
        }
        break;

      case 3:
        ack = handle_ctrl_pdu(nr, con, pkt + 2, len);
        break;
      }

      if(ack) {
        con->last_rx_seq = b0 & DATA_SN;
        con->stat.rx++;
      } else {
        con->stat.rx_qdrops++;
      }
    } else {
      con->stat.rx_bad_seq++;
    }
  } else {
    con->stat.rx_crc++;
  }

  // Prepare for transmit
  pbuf_t *pb = STAILQ_FIRST(&con->l2c.l2c_tx_queue);
  uint8_t *pkt;
  if(pb == NULL) {
    pkt = nr->nr_empty_packet;
    con->tx_dummy = 1;
  } else {
    pkt = pbuf_data(pb, 0);
    con->tx_dummy = 0;
    if(pb->pb_next)
      nr->nr_more_data |= 2;
  }

  uint8_t b0 = pkt[0];
  b0 &= ~(DATA_NESN | DATA_SN | DATA_MD);

  if(!con->last_rx_seq)
    b0 |= DATA_NESN;

  if(nr->nr_more_data & 2)
    b0 |= DATA_MD;

  b0 |= con->tx_seq & 1 ? DATA_SN : 0;
  pkt[0] = b0;

  reg_wr(RADIO_PACKETPTR, (intptr_t)pkt);

  uint32_t rxend = reg_rd(TIMER_BASE + TIMER_CC(2));
  uint32_t txstart = rxend + 110; // + 40µS ramp-up = 150µS T_IFS

  reg_wr(TIMER_BASE + TIMER_CC(0), txstart);
  reg_wr(DPPIC_CHENSET, (1 << DPPI_CH_TXEN)); // TIMER COMPARE0 -> RADIO TXEN

  con->next_timeout = con->next_anchor_point + con->timeout;

  if(n < -50 || n > 50)
    netlog("ble: Anchor adjust:%d @ %d", n, nr->nr_ll_state);

  nr->nr_ll_state = LL_CONNECTED_TX;
}


static void
conn_tx_done(nrf54l_radio_t *nr)
{
  reg_wr(DPPIC_CHENCLR, (1 << DPPI_CH_TXEN));

  if(nr->nr_more_data) {
    nr->nr_ll_state = LL_CONNECTED_RX_N;
    reg_wr(RADIO_PACKETPTR, (intptr_t)pbuf_data(nr->nr_pbuf, 0));
    reg_wr(RADIO_TASKS_RXEN, 1);
  } else {
    nr->nr_ll_state = LL_CONNECTED_IDLE;
  }
}


static void
conn_open_window(nrf54l_radio_t *nr)
{
  connection_t *con = &nr->nr_con;
  int delta = con->next_anchor_point - con->next_timeout;
  if(delta > 0) {
    netlog("ble: timo %d %d %d", delta, con->next_anchor_point,
           con->next_timeout);
    conn_terminate(nr, con, 0x08, "local");
  }

  if(con->termination_code && con->l2c.l2c_output == NULL) {
    conn_disconnect(nr, con->next_anchor_point);
    conn_fini(con);
    netlog("ble: Disconnected (code=0x%x)", con->termination_code);
    return;
  }

  con->eventCounter++;

  if(con->pending_chmask_valid &&
     con->pending_chmask_instant == con->eventCounter) {
    conn_update_channels(con->chmap, con->pending_chmask);
    con->pending_chmask_valid = 0;
  }

  conn_hop(nr, con);

  nr->nr_ll_state = LL_CONNECTED_RX_1;
  reg_wr(RADIO_PACKETPTR, (intptr_t)pbuf_data(nr->nr_pbuf, 0));
  reg_wr(RADIO_TASKS_RXEN, 1);

  arm_timer3(con->next_anchor_point + con->transmitWindowSize);
  con->next_anchor_point += con->connInterval;
}


static void
conn_close_window(nrf54l_radio_t *nr)
{
  connection_t *con = &nr->nr_con;

  reg_wr(RADIO_TASKS_DISABLE, 1);
  reg_wr(DPPIC_CHENCLR, (1 << DPPI_CH_TXEN));

  if(nr->nr_ll_state == LL_CONNECTED_RX_1) {
    // Never heard anything from central
    con->stat.rx_silent++;
    con->window_open_offset = 1000;
    arm_timer3(con->next_anchor_point - con->window_open_offset);
    nr->nr_ll_state = LL_CONNECTED_IDLE;
  } else {
    conn_open_window(nr);
  }
}


// RADIO END interrupt (RADIO_0)
void
irq_138(void)
{
  nrf54l_radio_t *nr = &g_radio;

  if(reg_rd(RADIO_EVENTS_END)) {
    reg_wr(RADIO_EVENTS_END, 0);

    switch(nr->nr_ll_state) {
    case LL_IDLE:
      break;

    case LL_ADV_TX:
      reg_wr(RADIO_PACKETPTR, (intptr_t)pbuf_data(nr->nr_pbuf, 0));
      reg_wr(RADIO_TASKS_RXEN, 1);
      nr->nr_ll_state = LL_ADV_RX;
      break;

    case LL_ADV_RX:
      if(reg_rd(RADIO_EVENTS_CRCOK)) {
        reg_wr(RADIO_EVENTS_CRCOK, 0);
        if(adv_rx(nr))
          break;
      }
      reg_wr(RADIO_PACKETPTR, (intptr_t)pbuf_data(nr->nr_pbuf, 0));
      reg_wr(RADIO_TASKS_RXEN, 1);
      break;

    case LL_CONNECTED_IDLE:
      break;

    case LL_CONNECTED_RX_1:
    case LL_CONNECTED_RX_N:
      conn_rx_done(nr);
      break;

    case LL_CONNECTED_TX:
      conn_tx_done(nr);
      break;

    default:
      panic("radio_irq: Invalid state %d", nr->nr_ll_state);
      break;
    }
  }
}


// TIMER10 COMPARE3 interrupt (connection window timing)
void
irq_133(void)
{
  nrf54l_radio_t *nr = &g_radio;

  if(reg_rd(TIMER_BASE + TIMER_EVENTS_COMPARE(3))) {
    reg_wr(TIMER_BASE + TIMER_EVENTS_COMPARE(3), 0);

    switch(nr->nr_ll_state) {
    case LL_IDLE:
    case LL_ADV_TX:
    case LL_ADV_RX:
      break;
    case LL_CONNECTED_IDLE:
      conn_open_window(nr);
      break;
    case LL_CONNECTED_TX:
    case LL_CONNECTED_RX_1:
    case LL_CONNECTED_RX_N:
      conn_close_window(nr);
      break;
    default:
      panic("radio: Invalid state %d (timer)", nr->nr_ll_state);
    }
  }
}


static void
nrf54l_radio_print_info(struct device *dev, struct stream *st)
{
  nrf54l_radio_t *nr = (nrf54l_radio_t *)dev;
  stprintf(st, "Addr: %02x:%02x:%02x:%02x:%02x:%02x  State: %s\n",
           nr->nr_addr[5], nr->nr_addr[4], nr->nr_addr[3],
           nr->nr_addr[2], nr->nr_addr[1], nr->nr_addr[0],
           state2str[nr->nr_ll_state]);

  if(nr->nr_ll_state < LL_CONNECTED_IDLE)
    return;

  connection_t *con = &nr->nr_con;

  stprintf(st, "Peer: %02x:%02x:%02x:%02x:%02x:%02x RSSI:%d\n",
           con->addr[5], con->addr[4], con->addr[3],
           con->addr[2], con->addr[1], con->addr[0], -con->rssi);

  stprintf(st, "windowSize: %d  interval: %d  timeout: %d\n",
           con->transmitWindowSize, con->connInterval, con->timeout);

  stprintf(st, "RX frames:%d  BadSeq:%d  Silent:%d  CRC:%d  Drops:%d\n",
           con->stat.rx, con->stat.rx_bad_seq, con->stat.rx_silent,
           con->stat.rx_crc, con->stat.rx_qdrops);
  stprintf(st, "TX frames:%d  Retransmissions:%d Qdepth:%d\n",
           con->stat.tx, con->stat.tx_retransmissions,
           con->l2c.l2c_tx_queue_len);

  l2cap_print(&con->l2c, st);
}


static const device_class_t nrf54l_ble_device_class = {
  .dc_class_name = "ble",
  .dc_print_info = nrf54l_radio_print_info,
};


static void
radio_timer_init(nrf54l_radio_t *nr)
{
  // DPPI: RADIO ADDRESS -> TIMER CAPTURE[1], RADIO END -> TIMER CAPTURE[2].
  reg_wr(RADIO_PUBLISH_ADDRESS, DPPI_EN | DPPI_CH_ADDRESS);
  reg_wr(TIMER_BASE + TIMER_SUBSCRIBE_CAPTURE(1), DPPI_EN | DPPI_CH_ADDRESS);
  reg_wr(RADIO_PUBLISH_END, DPPI_EN | DPPI_CH_END);
  reg_wr(TIMER_BASE + TIMER_SUBSCRIBE_CAPTURE(2), DPPI_EN | DPPI_CH_END);

  // DPPI: TIMER COMPARE[0] -> RADIO TXEN (channel enabled only when arming TX).
  reg_wr(TIMER_BASE + TIMER_PUBLISH_COMPARE(0), DPPI_EN | DPPI_CH_TXEN);
  reg_wr(RADIO_SUBSCRIBE_TXEN, DPPI_EN | DPPI_CH_TXEN);

  reg_wr(DPPIC_CHENSET, (1 << DPPI_CH_ADDRESS) | (1 << DPPI_CH_END));

  reg_wr(TIMER_BASE + TIMER_PRESCALER, TIMER_PRESCALER_1MHZ); // 1 MHz
  reg_wr(TIMER_BASE + TIMER_BITMODE, 3);                      // 32 bit
  reg_wr(TIMER_BASE + TIMER_INTENSET, 1 << 19);              // Compare3 -> IRQ

  irq_enable(TIMER_IRQ, IRQ_LEVEL_NET);
}


static void
buffers_avail(struct netif *ni)
{
  nrf54l_radio_t *nr = (nrf54l_radio_t *)ni;

  if(!nr->nr_pbuf) {
    nr->nr_pbuf = pbuf_make(0, 0);
    reg_wr(RADIO_PACKETPTR, (intptr_t)pbuf_data(nr->nr_pbuf, 0));
    softirq_raise(nr->nr_softirq_enter_adv);
  }
}


// Begin one advertising burst: HFXO is already running, so set up the channel,
// fire the radio, and arm the slow timer to close the burst (this is the work
// the nRF52 driver deferred to its xtal-ready callback).
static void
adv_start(nrf54l_radio_t *nr)
{
  radio_setup_for_adv(nr);
  memcpy(pbuf_data(nr->nr_pbuf, 0), nr->nr_adv_pkt, 2 + nr->nr_adv_pkt[1]);

  reg_wr(TIMER_BASE + TIMER_TASKS_CLEAR, 1);
  reg_wr(TIMER_BASE + TIMER_TASKS_START, 1);

  reg_wr(RADIO_TASKS_TXEN, 1);
  nr->nr_ll_state = LL_ADV_TX;
  timer_arm_abs(&nr->nr_slow_timer, clock_get_irq_blocked() + 4000);
}


static void
radio_slow_timer_cb(void *opaque, uint64_t now)
{
  nrf54l_radio_t *nr = opaque;

  int q = irq_forbid(IRQ_LEVEL_NET);
  switch(nr->nr_ll_state) {

  case LL_IDLE:
    if(nr->nr_pbuf)
      adv_start(nr);
    break;

  case LL_ADV_TX:
  case LL_ADV_RX:
    reg_wr(RADIO_TASKS_DISABLE, 1);

    if(nr->nr_announce_interval < 500000)
      nr->nr_announce_interval += 100;

    int delta = nr->nr_announce_interval;
    delta += rand() % (nr->nr_announce_interval >> 2);
    timer_arm_abs(&nr->nr_slow_timer, now + delta);

    nr->nr_ll_state = LL_IDLE;
    reg_wr(TIMER_BASE + TIMER_TASKS_STOP, 1);
    break;

  default:
    break;
  }

  irq_permit(q);
}


static void
radio_enter_adv(void *opaque)
{
  nrf54l_radio_t *nr = opaque;

  int q = irq_forbid(IRQ_LEVEL_CLOCK);
  nr->nr_announce_interval = 100000;
  timer_arm_abs(&nr->nr_slow_timer,
                clock_get_irq_blocked() + nr->nr_announce_interval);
  irq_permit(q);
}


void
nrf54l_radio_ble_init(const char *name)
{
  nrf54l_radio_t *nr = &g_radio;
  nr->nr_softirq_enter_adv = softirq_alloc(radio_enter_adv, nr);

  nr->nr_empty_packet[0] = 1;
  nr->nr_empty_packet[1] = 0;

  uint32_t deviceaddr0 = reg_rd(FICR_DEVICEADDR0);
  uint32_t deviceaddr1 = reg_rd(FICR_DEVICEADDR1);

  nr->nr_addr[0] = deviceaddr0;
  nr->nr_addr[1] = deviceaddr0 >> 8;
  nr->nr_addr[2] = deviceaddr0 >> 16;
  nr->nr_addr[3] = deviceaddr0 >> 24;
  nr->nr_addr[4] = deviceaddr1;
  nr->nr_addr[5] = (deviceaddr1 >> 8) | 0xc0; // random static

  build_adv_pkt(nr, name);

  nr->nr_bn.ni_buffers_avail = buffers_avail;
  nr->nr_announce_interval = 250000;

  // The radio needs the HFXO; keep it running while the stack is up.
  reg_wr(CLOCK_EVENTS_XOSTARTED, 0);
  reg_wr(CLOCK_TASKS_XOSTART, 1);
  while(!reg_rd(CLOCK_EVENTS_XOSTARTED)) {}

  radio_timer_init(nr);

  irq_enable(RADIO_IRQ, IRQ_LEVEL_NET);

  radio_init_ble();

  nr->nr_slow_timer.t_cb = radio_slow_timer_cb;
  nr->nr_slow_timer.t_opaque = nr;
  nr->nr_slow_timer.t_name = "radio";

  netif_init(&nr->nr_bn, "ble", &nrf54l_ble_device_class);
  netif_attach(&nr->nr_bn);
  printf("BLE radio initialized (%02x:%02x:%02x:%02x:%02x:%02x)\n",
         nr->nr_addr[5], nr->nr_addr[4], nr->nr_addr[3],
         nr->nr_addr[2], nr->nr_addr[1], nr->nr_addr[0]);
}
