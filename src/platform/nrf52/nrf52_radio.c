#include "nrf52_radio.h"
#include "nrf52_clk.h"
#include "nrf52_reg.h"
#include "nrf52_timer.h"
#include "nrf52_rng.h"
#include "nrf52_ppi.h"

#include "net/pbuf.h"
#include "net/ble/l2cap.h"
#include "net/ble/ble_proto.h"

#include <sys/param.h>

#include <mios/cli.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "irq.h"

#define CONN_WINDOW_OPEN_OFFSET 1000

#define ANNOUNCE_INTERVAL_FAST 100000
#define ANNOUNCE_INTERVAL_SLOW 1000000

#define RADIO_BASE 0x40001000

#define RADIO_TASKS_TXEN      (RADIO_BASE + 0x000)
#define RADIO_TASKS_RXEN      (RADIO_BASE + 0x004)
#define RADIO_TASKS_DISABLE   (RADIO_BASE + 0x010)
#define RADIO_EVENTS_END      (RADIO_BASE + 0x10c)
#define RADIO_EVENTS_DISABLED (RADIO_BASE + 0x110)
#define RADIO_EVENTS_CRCOK    (RADIO_BASE + 0x130)

#define RADIO_SHORTS       (RADIO_BASE + 0x200)
#define RADIO_INTENSET     (RADIO_BASE + 0x304)
#define RADIO_INTENCLR     (RADIO_BASE + 0x308)
#define RADIO_CRCSTATUS    (RADIO_BASE + 0x400)
#define RADIO_PACKETPTR    (RADIO_BASE + 0x504)
#define RADIO_FREQUENCY    (RADIO_BASE + 0x508)
#define RADIO_MODE         (RADIO_BASE + 0x510)
#define RADIO_PCNF0        (RADIO_BASE + 0x514)
#define RADIO_PCNF1        (RADIO_BASE + 0x518)
#define RADIO_BASE0        (RADIO_BASE + 0x51c)
#define RADIO_BASE1        (RADIO_BASE + 0x520)
#define RADIO_PREFIX0      (RADIO_BASE + 0x524)
#define RADIO_PREFIX1      (RADIO_BASE + 0x528)
#define RADIO_RXADDRESSES  (RADIO_BASE + 0x530)
#define RADIO_CRCCNF       (RADIO_BASE + 0x534)
#define RADIO_CRCPOLY      (RADIO_BASE + 0x538)
#define RADIO_CRCINIT      (RADIO_BASE + 0x53c)
#define RADIO_TIFS         (RADIO_BASE + 0x544)
#define RADIO_RSSISAMPLE   (RADIO_BASE + 0x548)
#define RADIO_STATE        (RADIO_BASE + 0x550)
#define RADIO_DATAWHITEIV  (RADIO_BASE + 0x554)
#define RADIO_MODECNF0     (RADIO_BASE + 0x650)

#define RADIO_POWER        (RADIO_BASE + 0xffc)

#define RADIO_SHORT_READY_START    (1 << 0)
#define RADIO_SHORT_END_DISABLE    (1 << 1)
#define RADIO_SHORT_DISABLED_TXEN  (1 << 2)
#define RADIO_SHORT_DISABLED_RXEN  (1 << 3)

#define RADIO_SHORT_RSSI_SAMPLING  ((1 << 4) | (1 << 8))

#define RADIO_IRQ_ENABLE           (1 << 0)
#define RADIO_IRQ_ADDRESS          (1 << 1)
#define RADIO_IRQ_PAYLOAD          (1 << 2)
#define RADIO_IRQ_END              (1 << 3)
#define RADIO_IRQ_DISABLED         (1 << 4)
#define RADIO_IRQ_DEVMATCH         (1 << 5)
#define RADIO_IRQ_DEVMIS           (1 << 6)
#define RADIO_IRQ_RSSIEND          (1 << 7)
#define RADIO_IRQ_BCMATCH          (1 << 10)
#define RADIO_IRQ_CRCOK            (1 << 12)
#define RADIO_IRQ_CRCERROR         (1 << 13)






// Link Layer State
typedef enum {
  LL_IDLE,
  LL_STANDBY,
  LL_ADV_TX,
  LL_ADV_RX,
  LL_CONNECTED_IDLE,
  LL_CONNECTED_RX,
  LL_CONNECTED_TX,
} ll_state_t;

const char state2str[7][8] = {
  "Idle",
  "Standby",
  "Adv_TX",
  "Adv_RX",
  "ConnIdl",
  "ConnRx",
  "ConnTx"
};



typedef struct connection {

  l2cap_t l2c;

  uint32_t access_addr;
  uint32_t crc_iv;

  // Time related variables are stored as µS

  // Constant variables
  uint32_t transmitWindowDelay;  // Depends on connect PDU
  uint32_t transmitWindowSize;   // from lldata
  uint32_t transmitWindowOffset; // from lldata
  uint32_t connInterval;
  uint32_t latency;
  uint32_t timeout;

  uint32_t next_anchor_point;
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
  } stat;

  uint8_t addr[6];

  uint8_t anything;

} connection_t;


typedef struct nrf52_radio {

  netif_t nr_bn;

  ll_state_t nr_ll_state;

  pbuf_t *nr_pbuf;

  void (*status_cb)(int status);

  connection_t nr_con;

  uint32_t nr_adv_timeout;
  uint32_t nr_announce_interval;

  uint8_t nr_addr[6];

  uint8_t nr_adv_pkt[2 + 6 + 31];

  uint8_t nr_empty_packet[2];

  uint8_t nr_more_data;
  uint8_t nr_resync;

  uint8_t nr_power_mode;
  uint8_t nr_adv_ch;

} nrf52_radio_t;


static nrf52_radio_t g_radio;

static void
build_adv_pkt(nrf52_radio_t *nr, const char *name)
{
  const size_t namelen = strlen(name);

  uint8_t *p = nr->nr_adv_pkt;
  p[0] = ADV_IND | ADV_TXADD; // Random address
  p[1] = 6 + namelen + 2;
  memcpy(p + 2, nr->nr_addr, 6);
  p[8] = namelen + 1;
  p[9] = 8;
  memcpy(p + 10, name, namelen);
}


static const uint8_t channel_to_freq[40] = { // 2400 Mhz + indexed value
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
  reg_wr(RADIO_DATAWHITEIV, channel);
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

  reg_wr(RADIO_RXADDRESSES, 0x10001);

  const int lflen = 8;
  const int s0len = 1;
  const int s1len = 0;

  reg_wr(RADIO_PCNF0,
         (lflen << 0)  | // Length of LEN in bits
         (s0len << 8)  | // Length of S0 in bytes
         (s1len << 16) | // Length of S1 in bytes
         0);

  const int maxlen = PBUF_DATA_SIZE - 2;
  const int statlen = 0;
  reg_wr(RADIO_PCNF1,
         (maxlen << 0) |
         (statlen << 8) |
         (3 << 16) | // balen = 4 (3 + 1)
         (1 << 25)); // Enable whitening

  reg_wr(RADIO_MODECNF0,
         //         (1 << 0) | // Fast ramp-up (Does not work with TIFS)
         (2 << 8) | // Transmit center frequency when not started
         0);

  reg_wr(RADIO_INTENSET, RADIO_IRQ_DISABLED);
}


static void
radio_setup_for_adv(nrf52_radio_t *nr)
{
  reg_wr(RADIO_SHORTS,
         RADIO_SHORT_RSSI_SAMPLING |
         RADIO_SHORT_READY_START |
         RADIO_SHORT_END_DISABLE);

  reg_wr(RADIO_MODECNF0,
         (1 << 0) | // Fast ramp-up (Does not work with TIFS)
         (2 << 8) | // Transmit center frequency when not started
         0);
  reg_wr(RADIO_TIFS, 0);

  nr->nr_adv_ch++;
  if(nr->nr_adv_ch == 3)
    nr->nr_adv_ch = 0;

  select_channel(37 + nr->nr_adv_ch, 0x555555, 0x8e89bed6);
}

static void
radio_arm_for_advertisement(nrf52_radio_t *nr)
{
  nr->nr_adv_timeout += nr->nr_announce_interval + reg_rd(RNG_VALUE) * 32;
  reg_wr(TIMER0_BASE + TIMER_CC(3), nr->nr_adv_timeout);
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
  pbuf_pullup(pb, pb->pb_pktlen);

  assert(pb->pb_next == NULL);

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
conn_disconnect(nrf52_radio_t *nr, uint32_t now)
{
  nr->nr_ll_state = LL_STANDBY;
  nr->nr_adv_timeout = now;
  radio_setup_for_adv(nr);
  radio_arm_for_advertisement(nr);
  nr->status_cb(0);
}


static void
conn_hop(nrf52_radio_t *nr, connection_t *con)
{
  // 4.5.8.2 Channel Selection algorithm #1

  if(nr->nr_resync) {
    con->stat.rx_silent++;
  }

  uint8_t ch = (con->last_unmapped_channel + con->hop_increment) % 37;
  con->last_unmapped_channel = ch;
  select_channel(con->chmap[ch], con->crc_iv, con->access_addr);
}


static int
handle_CONNECT_IND(nrf52_radio_t *nr, const uint8_t *pkt)
{
  const struct lldata *lld = (struct lldata *)(pkt + 14);
  connection_t *con = &nr->nr_con;

  if(l2cap_connect(&con->l2c))
    return 0;

  memcpy(con->addr, pkt + 2, 6);

  conn_init(con, lld);

  uint32_t rx_end_time = reg_rd(TIMER0_BASE + TIMER_CC(2));

  con->next_anchor_point =
    rx_end_time + con->transmitWindowDelay +
    con->transmitWindowOffset;

  reg_wr(TIMER0_BASE + TIMER_CC(3),
         con->next_anchor_point - CONN_WINDOW_OPEN_OFFSET);

  nr->nr_ll_state = LL_CONNECTED_IDLE;

  reg_wr(RADIO_SHORTS,
         RADIO_SHORT_READY_START |
         RADIO_SHORT_END_DISABLE |
         RADIO_SHORT_DISABLED_TXEN);

  reg_wr(RADIO_TIFS, 150);

  reg_wr(RADIO_MODECNF0,
         (2 << 8) | // Transmit center frequency when not started
         0);

  con->next_timeout = con->next_anchor_point  + con->timeout;
  netlog("ble: Connected to %02x:%02x:%02x:%02x:%02x:%02x RSSI:%d hop:%d (on %d)",
         pkt[2 + 5],
         pkt[2 + 4],
         pkt[2 + 3],
         pkt[2 + 2],
         pkt[2 + 1],
         pkt[2 + 0],
         -reg_rd(RADIO_RSSISAMPLE),
         con->hop_increment,
         37 + nr->nr_adv_ch);

  memset(&con->stat, 0, sizeof(con->stat));
  nr->status_cb(NRF52_BLE_STATUS_CONNECTED);

  return 1;
}


static int
adv_rx(nrf52_radio_t *nr)
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
  // Allocate response packet
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
handle_ll_channel_map_ind(connection_t *con,
                          const uint8_t *req,
                          int reqlen)
{
  if(reqlen < 7)
    return 1;

  memcpy(&con->pending_chmask, req, 5);
  memcpy(&con->pending_chmask_instant, req + 5, 2);
  con->pending_chmask_valid = 1;
  return 1;
}


static int
handle_ll_version_ind(connection_t *con,
                      const uint8_t *req,
                      int reqlen)
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
conn_terminate(nrf52_radio_t *nr, connection_t *con, uint8_t code,
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
handle_ctrl_pdu(nrf52_radio_t *nr,
                connection_t *con,
                const uint8_t *req,
                int reqlen)
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
  default:
    return handle_unknown_ctrlop(con, req[0]);
  }
}


static void
log_data_pdu(const uint8_t *pkt, const char *prefix)
{
  return;
  netlog_hexdump(prefix, pkt, pkt[1] + 2);
}



static void
conn_rx_done(nrf52_radio_t *nr)
{
  connection_t *con = &nr->nr_con;

  if(nr->nr_resync) {
    nr->nr_resync = 0;

    const uint32_t anchor_point = reg_rd(TIMER0_BASE + TIMER_CC(1));
    con->next_anchor_point = anchor_point + con->connInterval;
    con->next_timeout = con->next_anchor_point + con->timeout;

    reg_wr(TIMER0_BASE + TIMER_CC(3),
           con->next_anchor_point - CONN_WINDOW_OPEN_OFFSET);
  }

  const uint8_t *rx_pkt = NULL;
  nr->nr_more_data = 0;
  if(reg_rd(RADIO_CRCSTATUS)) {
    uint8_t *pkt = pbuf_data(nr->nr_pbuf, 0);
    rx_pkt = pkt;

    const uint8_t b0 = pkt[0];
    const uint8_t len = pkt[1];

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

      // Advance sequence
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
            // No RX buffers avaiable, NAK frame
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
    // Nothing to send, transmit dummy frame
    pkt = nr->nr_empty_packet;
    con->tx_dummy = 1;
  } else {
    pkt = pbuf_data(pb, 0);
    con->tx_dummy = 0;
    if(pb->pb_next)
      nr->nr_more_data |= 2;
  }

  // Update header byte
  uint8_t b0 = pkt[0];
  b0 &= ~(DATA_NESN | DATA_SN | DATA_MD);

  // If we received seq0, say we expect seq1
  if(!con->last_rx_seq)
    b0 |= DATA_NESN;

  if(nr->nr_more_data & 2)
    b0 |= DATA_MD;

  b0 |= con->tx_seq & 1 ? DATA_SN : 0;
  pkt[0] = b0;

  reg_wr(RADIO_PACKETPTR, (intptr_t)pkt);
  reg_wr(RADIO_SHORTS, 0b11); // Ready->start, end->disable
  nr->nr_ll_state = LL_CONNECTED_TX;

  if(rx_pkt)
    log_data_pdu(rx_pkt, "RX");
  log_data_pdu(pkt, "TX");
}


static void
conn_tx_done(nrf52_radio_t *nr)
{
  reg_wr(RADIO_PACKETPTR, (intptr_t)pbuf_data(nr->nr_pbuf, 0));

  if(nr->nr_more_data) {
    reg_wr(RADIO_SHORTS, 0b111); // Ready->start, end->disable, disabled->txen
    reg_wr(RADIO_TASKS_RXEN, 1);
    nr->nr_ll_state = LL_CONNECTED_RX;
  } else {
    nr->nr_ll_state = LL_CONNECTED_IDLE;
  }
}


static void
conn_open_window(nrf52_radio_t *nr)
{
  connection_t *con = &nr->nr_con;

  if(con->next_anchor_point > con->next_timeout) {
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

  nr->nr_resync = 1;
  reg_wr(RADIO_SHORTS, 0b111); // Ready->start, end->disable, disabled->txen
  reg_wr(RADIO_TASKS_RXEN, 1);

  nr->nr_ll_state = LL_CONNECTED_RX;

  reg_wr(TIMER0_BASE + TIMER_CC(3),
         con->next_anchor_point + con->transmitWindowSize);

  con->next_anchor_point += con->connInterval;
}


#if 0
static void
conn_close_window(nrf52_radio_t *nr)
{
  connection_t *con = &nr->nr_con;

  reg_wr(RADIO_SHORTS, 0b11); // Ready->start, end->disable
  reg_wr(RADIO_TASKS_DISABLE, 1);

  nr->nr_ll_state = LL_CONNECTED_IDLE;
  reg_wr(TIMER0_BASE + TIMER_CC(3),
         con->next_anchor_point - CONN_WINDOW_OPEN_OFFSET);
}
#endif

/*********************
 * Radio IRQ
 */
void
irq_1(void)
{
  nrf52_radio_t *nr = &g_radio;

  if(reg_rd(RADIO_EVENTS_DISABLED)) {
    reg_wr(RADIO_EVENTS_DISABLED, 0);

    switch(nr->nr_ll_state) {
    case LL_IDLE:
    case LL_STANDBY:
      break;

    case LL_ADV_TX:
      reg_wr(RADIO_TASKS_RXEN, 1);
      nr->nr_ll_state = LL_ADV_RX;
      break;

    case LL_ADV_RX:
      if(reg_rd(RADIO_EVENTS_CRCOK)) {
        reg_wr(RADIO_EVENTS_CRCOK, 0);
        if(adv_rx(nr))
          break;
      }
      reg_wr(RADIO_TASKS_RXEN, 1);
      break;

    case LL_CONNECTED_IDLE:
      break;

    case LL_CONNECTED_RX:
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


/*********************
 * Timer IRQ
 */
void
irq_8(void)
{
  nrf52_radio_t *nr = &g_radio;

  if(reg_rd(TIMER0_BASE + TIMER_EVENTS_COMPARE(3))) {
    reg_wr(TIMER0_BASE + TIMER_EVENTS_COMPARE(3), 0);

    switch(nr->nr_ll_state) {
    case LL_STANDBY:
      if(nr->nr_power_mode == NRF52_RADIO_PWR_OFF) {
        nr->nr_ll_state = LL_IDLE;
        reg_wr(TIMER0_BASE + TIMER_TASKS_STOP, 1);
        netlog("ble: Idle");
        break;
      }

    case LL_IDLE:
      radio_setup_for_adv(nr);
      memcpy(pbuf_data(nr->nr_pbuf, 0), nr->nr_adv_pkt, 2 + nr->nr_adv_pkt[1]);
      reg_wr(RADIO_TASKS_TXEN, 1);
      nr->nr_ll_state = LL_ADV_TX;
      nr->nr_adv_timeout += 4000;
      reg_wr(TIMER0_BASE + TIMER_CC(3), nr->nr_adv_timeout);
      break;

    case LL_ADV_TX:
    case LL_ADV_RX:
      reg_wr(RADIO_TASKS_DISABLE, 1);
      nr->nr_adv_timeout += nr->nr_announce_interval + reg_rd(RNG_VALUE) * 32;
      reg_wr(TIMER0_BASE + TIMER_CC(3), nr->nr_adv_timeout);
      nr->nr_ll_state = LL_STANDBY;
      break;

    case LL_CONNECTED_IDLE:
    case LL_CONNECTED_TX:
    case LL_CONNECTED_RX:
      conn_open_window(nr);
      break;

    default:
      panic("radio: Invalid state %d (timer)", nr->nr_ll_state);
    }
  }
}


static void
nrf52_radio_print_info(struct device *dev, struct stream *st)
{
  nrf52_radio_t *nr = (nrf52_radio_t *)dev;
  stprintf(st, "\tAddr: %02x:%02x:%02x:%02x:%02x:%02x  State: %s\n",
           nr->nr_addr[5],
           nr->nr_addr[4],
           nr->nr_addr[3],
           nr->nr_addr[2],
           nr->nr_addr[1],
           nr->nr_addr[0],
           state2str[nr->nr_ll_state]);

  if(nr->nr_ll_state < LL_CONNECTED_IDLE)
    return;

  connection_t *con = &nr->nr_con;

  stprintf(st, "\tPeer: %02x:%02x:%02x:%02x:%02x:%02x\n",
           con->addr[5],
           con->addr[4],
           con->addr[3],
           con->addr[2],
           con->addr[1],
           con->addr[0]);

  stprintf(st, "\twindowSize: %d  interval: %d  timeout: %d\n",
           con->transmitWindowSize,
           con->connInterval,
           con->timeout);

  stprintf(st, "\tChannels: ");
  for(int i = 0; i < 37; i++) {
    stprintf(st, "%c", con->chmap[i] == i ? 'X' : '_');
  }
  stprintf(st, "\n");


  stprintf(st, "\tRX frames:%d  BadSeq:%d  Silent:%d  CRC:%d  Drops:%d\n",
           con->stat.rx,
           con->stat.rx_bad_seq,
           con->stat.rx_silent,
           con->stat.rx_crc,
           con->stat.rx_qdrops);

  stprintf(st, "\tTX frames:%d  Retransmissions:%d Qdepth:%d\n",
           con->stat.tx, con->stat.tx_retransmissions,
           con->l2c.l2c_tx_queue_len);
}


static const device_class_t nrf52_ble_device_class = {
  .dc_print_info = nrf52_radio_print_info,
};


static void
radio_timer_init(nrf52_radio_t *nr)
{
  reg_wr(PPI_CHENSET, (1 << 26)); // RADIO_EVENT_ADDRESS -> TIMER0_CAPTURE1
  reg_wr(PPI_CHENSET, (1 << 27)); // RADIO_EVENT_END     -> TIMER0_CAPTURE2

  reg_wr(TIMER0_BASE + TIMER_BITMODE, 3);        // 32 bit width
  reg_wr(TIMER0_BASE + TIMER_INTENSET, 1 << 19); // Compare3 -> IRQ

  reg_wr(TIMER0_BASE + TIMER_TASKS_CLEAR, 1);
  reg_wr(TIMER0_BASE + TIMER_CC(3), nr->nr_announce_interval);
  reg_wr(TIMER0_BASE + TIMER_TASKS_START, 1);

  irq_enable(TIMER0_IRQ, IRQ_LEVEL_NET);
}





static void
buffers_avail(struct netif *ni)
{
  nrf52_radio_t *nr = (nrf52_radio_t *)ni;

  if(!nr->nr_pbuf) {
    nr->nr_pbuf = pbuf_make(0, 0);
    reg_wr(RADIO_PACKETPTR, (intptr_t)pbuf_data(nr->nr_pbuf, 0));
    radio_arm_for_advertisement(nr);
  }
}


void
nrf52_radio_ble_init(const char *name, void (*status_cb)(int flags))
{
  nrf52_xtal_enable();

  nrf52_radio_t *nr = &g_radio;
  nr->status_cb = status_cb;

  nr->nr_empty_packet[0] = 1;
  nr->nr_empty_packet[1] = 0;

  uint32_t deviceaddr0 = reg_rd(0x100000a4);
  uint32_t deviceaddr1 = reg_rd(0x100000a8);

  nr->nr_addr[0] = deviceaddr0;
  nr->nr_addr[1] = deviceaddr0 >> 8;
  nr->nr_addr[2] = deviceaddr0 >> 16;
  nr->nr_addr[3] = deviceaddr0 >> 24;
  nr->nr_addr[4] = deviceaddr1;
  nr->nr_addr[5] = (deviceaddr1 >> 8) | 0xc0;

  build_adv_pkt(nr, name);

  nr->nr_bn.ni_buffers_avail = buffers_avail;
  nr->nr_announce_interval = ANNOUNCE_INTERVAL_FAST;

  radio_timer_init(nr);
  irq_enable(1, IRQ_LEVEL_NET);

  radio_init_ble();
  netif_attach(&nr->nr_bn, "ble", &nrf52_ble_device_class);

  printf("BLE radio initialized\n");
}



void
nrf52_radio_power_mode(int mode)
{
  nrf52_radio_t *nr = &g_radio;

  if(nr->nr_power_mode == mode)
    return;

  int q = irq_forbid(IRQ_LEVEL_NET);

  switch(mode) {
  case NRF52_RADIO_PWR_HIGH:
    nr->nr_announce_interval = ANNOUNCE_INTERVAL_FAST;
    netlog("ble: Power mode: %s", "High");
    break;
  case NRF52_RADIO_PWR_LOW:
    nr->nr_announce_interval = ANNOUNCE_INTERVAL_SLOW;
    netlog("ble: Power mode: %s", "Low");
    break;
  }

  if(nr->nr_power_mode == NRF52_RADIO_PWR_OFF && mode != NRF52_RADIO_PWR_OFF) {
    radio_arm_for_advertisement(nr);
    reg_wr(TIMER0_BASE + TIMER_TASKS_START, 1);
    netlog("ble: Restarted");
  }
  nr->nr_power_mode = mode;

  irq_permit(q);
}
