// BLE controller glue for Nordic's SoftDevice Controller (binary blob,
// sdk-nrfxlib). The SDC sits below the Bluetooth HCI boundary; everything
// from l2cap up is the shared mios host stack.
//
// Execution model: every SDC/MPSL "low priority" API must be called from one
// execution priority. That priority is IRQ_LEVEL_NET here: the MPSL low
// priority interrupt runs at NET level and drains HCI events/data,
// and all other call sites (init, l2cap output path) block NET around their
// calls. The time-critical radio scheduling runs in NVIC priority-0
// interrupts owned by MPSL, which mios never masks (BASEPRI levels >= 1).

#include "nrf_sdc.h"

#include "net/pbuf.h"
#include "net/netif.h"
#include "net/ble/l2cap.h"
#include "net/ble/smp.h"

#include <mios/mios.h>

#include <string.h>
#include <stdio.h>

#include "irq.h"

#include "mpsl.h"
#include "sdc.h"
#include "sdc_soc.h"
#include "sdc_hci.h"
#include "sdc_hci_cmd_le.h"
#include "sdc_hci_cmd_controller_baseband.h"
#include "sdc_hci_evt.h"

// Link layer packet size: matched to our pbufs so an ACL fragment always
// fits one pbuf (the SDC negotiates DLE up to this).
#define SDC_PKT_SIZE LLMTU
#define SDC_TX_COUNT 3
#define SDC_RX_COUNT 2

// Controller memory pool, sized with the compile-time worst-case macros for
// our configuration. sdc_cfg_set() reports the actual requirement at init
// and we verify it fits.
static uint8_t sdc_mem[SDC_MEM_PER_PERIPHERAL_LINK(SDC_PKT_SIZE, SDC_PKT_SIZE,
                                                   SDC_TX_COUNT, SDC_RX_COUNT) +
                       SDC_MEM_PERIPHERAL_LINKS_SHARED +
                       SDC_MEM_PER_ADV_SET(SDC_DEFAULT_ADV_BUF_SIZE) +
                       SDC_MEM_FAL(SDC_DEFAULT_FAL_SIZE)]
  __attribute__((aligned(8)));

typedef struct sdc_ble {

  netif_t sb_ni;

  struct {
    l2cap_t l2c; // must be first: l2c_output casts back
    uint16_t handle;
    uint16_t interval;   // units of 1.25 ms
    uint16_t timeout;    // units of 10 ms
    uint8_t peer[6];
    uint8_t tx_phy, rx_phy;
    uint8_t connected;
    uint8_t encrypted;
  } con;

  uint8_t sb_tx_credits;  // ACL packets the controller can accept right now
  uint8_t sb_tx_ceiling;  // from LE Read Buffer Size

  uint8_t sb_advertising;
  uint8_t sb_addr[6];

  struct {
    uint32_t rx;
    uint32_t tx;
    uint32_t rx_drops;    // pbuf exhaustion
    uint32_t rx_stale;    // ACL data for a handle we do not know
    uint32_t rx_oversize; // ACL data larger than our LL packet size
    uint32_t events;
    uint32_t swi_runs;    // low priority interrupt invocations
  } stat;

  const char *sb_name;

} sdc_ble_t;

static sdc_ble_t g_sdc;

static void
sdc_fault(const char *file, uint32_t line)
{
  panic("sdc fault %s:%d", file, line);
}

static void
sdc_rand_poll(uint8_t *buf, uint8_t len)
{
  nrf_trng_read(buf, len);
}


// Entropy for the SMP pairing nonce (overrides the weak default in smp.c).
void
ble_rand(void *out, unsigned int len)
{
  nrf_trng_read(out, len);
}


// Answer a pending controller LTK request (SMP -> controller). NULL rejects.
static void
sdc_ltk_reply(l2cap_t *l2c, const uint8_t *ltk)
{
  sdc_ble_t *sb = &g_sdc;

  if(ltk == NULL) {
    const sdc_hci_cmd_le_long_term_key_request_negative_reply_t neg = {
      .conn_handle = sb->con.handle,
    };
    sdc_hci_cmd_le_long_term_key_request_negative_reply_return_t ret;
    sdc_hci_cmd_le_long_term_key_request_negative_reply(&neg, &ret);
    return;
  }

  sdc_hci_cmd_le_long_term_key_request_reply_t rep = {
    .conn_handle = sb->con.handle,
  };
  memcpy(rep.long_term_key, ltk, 16);
  sdc_hci_cmd_le_long_term_key_request_reply_return_t ret;
  sdc_hci_cmd_le_long_term_key_request_reply(&rep, &ret);
}


static uint8_t
sdc_adv_enable(uint8_t on)
{
  const sdc_hci_cmd_le_set_adv_enable_t en = { .adv_enable = on };
  uint8_t status = sdc_hci_cmd_le_set_adv_enable(&en);
  if(!status)
    g_sdc.sb_advertising = on;
  return status;
}


// --- TX: l2cap fragments -> HCI ACL ----------------------------------------
// l2cap queues raw fragments (PBUF_SOP marks an SDU start) on l2c_tx_queue;
// the pump converts to ACL packets while the controller has buffer credits.
// Runs at IRQ_LEVEL_NET only.

static void
sdc_tx_pump(sdc_ble_t *sb)
{
  static uint8_t txbuf[4 + SDC_PKT_SIZE];

  while(sb->con.connected && sb->sb_tx_credits) {
    pbuf_t *pb = STAILQ_FIRST(&sb->con.l2c.l2c_tx_queue);
    if(pb == NULL)
      break;
    STAILQ_REMOVE_HEAD(&sb->con.l2c.l2c_tx_queue, pb_link);
    sb->con.l2c.l2c_tx_queue_len--;
    // pb_next aliases the queue linkage; detach so the free below does not
    // walk into (and free) the rest of the queue.
    pb->pb_next = NULL;

    // PB flag: 0b00 = first non-flushable, 0b01 = continuation
    const uint16_t hf = sb->con.handle | (pb->pb_flags & PBUF_SOP ? 0 : 0x1000);
    const uint16_t len = pb->pb_pktlen;
    txbuf[0] = hf;
    txbuf[1] = hf >> 8;
    txbuf[2] = len;
    txbuf[3] = len >> 8;
    memcpy(txbuf + 4, pbuf_data(pb, 0), len);
    pbuf_free_irq_blocked(pb);

    if(sdc_hci_data_put(txbuf))
      break;
    sb->sb_tx_credits--;
    sb->stat.tx++;
  }
}


static void
sdc_conn_output(struct l2cap *self, struct pbuf *pb)
{
  sdc_ble_t *sb = &g_sdc;

  if(pb == NULL) {
    // l2cap layer closed
    self->l2c_output = NULL;
    return;
  }

  // Fragments may arrive as pbuf chains; flatten so the pump can copy the
  // whole ACL payload from one buffer (fragments are <= LLMTU by contract).
  if(pbuf_pullup(pb, pb->pb_pktlen))
    panic("pullup failed");

  int q = irq_forbid(IRQ_LEVEL_NET);
  if(sb->con.connected) {
    STAILQ_INSERT_TAIL(&self->l2c_tx_queue, pb, pb_link);
    self->l2c_tx_queue_len++;
    sdc_tx_pump(sb);
  } else {
    pbuf_free_irq_blocked(pb);
  }
  irq_permit(q);
}


// --- RX: HCI events and ACL data -------------------------------------------
// All of this runs in the MPSL low priority interrupt at IRQ_LEVEL_NET.

static void
sdc_handle_conn_complete(sdc_ble_t *sb, const uint8_t *p)
{
  const sdc_hci_subevent_le_conn_complete_t *cc = (const void *)p;

  if(cc->status)
    return;

  sb->con.handle = cc->conn_handle;
  sb->con.interval = cc->conn_interval;
  sb->con.timeout = cc->supervision_timeout;
  sb->con.tx_phy = 1;
  sb->con.rx_phy = 1;
  sb->con.encrypted = 0;
  memcpy(sb->con.peer, cc->peer_address, 6);

  sb->con.l2c.l2c_output = sdc_conn_output;
  sb->con.l2c.l2c_ltk_reply = sdc_ltk_reply;
  STAILQ_INIT(&sb->con.l2c.l2c_tx_queue);
  sb->con.l2c.l2c_tx_queue_len = 0;

  // Addresses for the pairing crypto (HCI order = LSB first).
  memcpy(sb->con.l2c.l2c_peer_addr, cc->peer_address, 6);
  sb->con.l2c.l2c_peer_addr_type = cc->peer_address_type;
  memcpy(sb->con.l2c.l2c_our_addr, sb->sb_addr, 6);
  sb->con.l2c.l2c_our_addr_type = 1; // static random

  if(l2cap_connect(&sb->con.l2c))
    return;

  sb->con.connected = 1;
  sb->sb_advertising = 0; // controller stops advertising on connect

  netlog("ble: Connected to %02x:%02x:%02x:%02x:%02x:%02x interval:%d",
         cc->peer_address[5], cc->peer_address[4], cc->peer_address[3],
         cc->peer_address[2], cc->peer_address[1], cc->peer_address[0],
         cc->conn_interval);
}


static void
sdc_handle_disconn(sdc_ble_t *sb, const uint8_t *p)
{
  const sdc_hci_event_disconn_complete_t *dc = (const void *)p;

  if(!sb->con.connected || dc->conn_handle != sb->con.handle)
    return;

  netlog("ble: Disconnected (reason=0x%x)", dc->reason);

  sb->con.connected = 0;
  l2cap_disconnect(&sb->con.l2c);
  pbuf_free_queue_irq_blocked(&sb->con.l2c.l2c_tx_queue);
  sb->con.l2c.l2c_tx_queue_len = 0;
  sb->sb_tx_credits = sb->sb_tx_ceiling;

  sdc_adv_enable(1);
}


static void
sdc_handle_event(sdc_ble_t *sb, const uint8_t *buf)
{
  const uint8_t code = buf[0];
  const uint8_t *p = buf + 2;

  switch(code) {
  case SDC_HCI_EVENT_LE_META:
    switch(p[0]) {
    case SDC_HCI_SUBEVENT_LE_CONN_COMPLETE:
      sdc_handle_conn_complete(sb, p + 1);
      break;
    case SDC_HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE: {
      const sdc_hci_subevent_le_phy_update_complete_t *pu =
        (const void *)(p + 1);
      if(!pu->status) {
        sb->con.tx_phy = pu->tx_phy;
        sb->con.rx_phy = pu->rx_phy;
      }
      break;
    }
    case SDC_HCI_SUBEVENT_LE_LONG_TERM_KEY_REQUEST: {
      const sdc_hci_subevent_le_long_term_key_request_t *ltk =
        (const void *)(p + 1);
      smp_ltk_request(&sb->con.l2c, ltk->random_number,
                      ltk->encrypted_diversifier);
      break;
    }
    default:
      break;
    }
    break;

  case SDC_HCI_EVENT_ENCRYPTION_CHANGE: {
    const sdc_hci_event_encryption_change_t *ec = (const void *)p;
    const int on = !ec->status && ec->encryption_enabled;
    sb->con.encrypted = on;
    smp_encryption_changed(&sb->con.l2c, on);
    break;
  }

  case SDC_HCI_EVENT_DISCONN_COMPLETE:
    sdc_handle_disconn(sb, p);
    break;

  case SDC_HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS: {
    const sdc_hci_event_number_of_completed_packets_t *ev = (const void *)p;
    for(int i = 0; i < ev->num_handles; i++)
      sb->sb_tx_credits += ev->handles[i].num_completed_packets;
    sdc_tx_pump(sb);
    break;
  }

  default:
    break;
  }
}


static void
sdc_handle_data(sdc_ble_t *sb, const uint8_t *buf)
{
  const uint16_t hf = buf[0] | (buf[1] << 8);
  const uint16_t handle = hf & 0xfff;
  const uint8_t pb_flag = (hf >> 12) & 3;
  const uint16_t len = buf[2] | (buf[3] << 8);

  if(!sb->con.connected || handle != sb->con.handle) {
    sb->stat.rx_stale++;
    return;
  }
  if(len > SDC_PKT_SIZE) {
    sb->stat.rx_oversize++;
    return;
  }

  pbuf_t *pb = pbuf_make_irq_blocked(0, 0);
  if(pb == NULL) {
    sb->stat.rx_drops++;
    return;
  }

  // Same layout the native link layer hands to l2cap_input: payload at
  // offset 2 (where the LL header would sit), pktlen left for l2cap_splice.
  pb->pb_pktlen = 0;
  pb->pb_offset = 2;
  pb->pb_buflen = len;
  // PB flag 0b10 = first fragment (controller to host), 0b01 = continuation
  pb->pb_flags = pb_flag == 2 ? PBUF_SOP : 0;
  memcpy(pbuf_data(pb, 0), buf + 4, len);
  sb->stat.rx++;
  l2cap_input(&sb->con.l2c, pb);
}


// MPSL low priority interrupt (IRQ_LEVEL_NET): controller housekeeping,
// then drain the HCI message queue.
static void
sdc_low_prio(void)
{
  static uint8_t msgbuf[HCI_MSG_BUFFER_MAX_SIZE];
  sdc_ble_t *sb = &g_sdc;

  sb->stat.swi_runs++;
  mpsl_low_priority_process();

  uint8_t msg_type;
  while(sdc_hci_get(msgbuf, &msg_type) == 0) {
    if(msg_type == SDC_HCI_MSG_TYPE_EVT) {
      sb->stat.events++;
      sdc_handle_event(sb, msgbuf);
    } else if(msg_type == SDC_HCI_MSG_TYPE_DATA) {
      sdc_handle_data(sb, msgbuf);
    }
  }
}


static void
sdc_hci_signal(void)
{
  // New HCI messages are available. We may already be inside sdc_low_prio's
  // drain loop; pending the interrupt again is always safe.
  nrf_mpsl_kick();
}


// --- Init -------------------------------------------------------------------

static void
sdc_print_info(struct device *dev, struct stream *st)
{
  sdc_ble_t *sb = (sdc_ble_t *)dev;

  stprintf(st, "BLE: %02x:%02x:%02x:%02x:%02x:%02x  (SDC)  State: %s\n",
           sb->sb_addr[5], sb->sb_addr[4], sb->sb_addr[3],
           sb->sb_addr[2], sb->sb_addr[1], sb->sb_addr[0],
           sb->con.connected ? "Connected" :
           sb->sb_advertising ? "Advertising" : "Idle");

  if(sb->con.connected) {
    stprintf(st, "  Peer: %02x:%02x:%02x:%02x:%02x:%02x  %s\n",
             sb->con.peer[5], sb->con.peer[4], sb->con.peer[3],
             sb->con.peer[2], sb->con.peer[1], sb->con.peer[0],
             sb->con.encrypted ? "ENCRYPTED" : "unencrypted");
    stprintf(st, "  interval: %dus  timeout: %dus  PHY rx:%s tx:%s\n",
             sb->con.interval * 1250, sb->con.timeout * 10000,
             sb->con.rx_phy == 2 ? "2M" : "1M",
             sb->con.tx_phy == 2 ? "2M" : "1M");
    stprintf(st, "  RX:%d  Drops:%d  TX:%d  Credits:%d  Qdepth:%d\n",
             sb->stat.rx, sb->stat.rx_drops, sb->stat.tx,
             sb->sb_tx_credits, sb->con.l2c.l2c_tx_queue_len);
    l2cap_print(&sb->con.l2c, st);
  }
  stprintf(st, "  Stale:%d  Oversize:%d  Events:%d  SWI:%d\n",
           sb->stat.rx_stale, sb->stat.rx_oversize,
           sb->stat.events, sb->stat.swi_runs);
}


static const device_class_t sdc_device_class = {
  .dc_class_name = "radio",
  .dc_print_info = sdc_print_info,
};


// Called from board init (constructor 8xx, interrupts still masked): just
// record the name. The controller is brought up on the main thread.
void
nrf_ble_init(const char *name)
{
  g_sdc.sb_name = name;
}


static void
sdc_setup_hci(sdc_ble_t *sb)
{
  uint8_t status;

  // Event routing: Disconnection Complete (bit 4), LE Meta (bit 61); within
  // LE Meta: Connection Complete (0), Connection Update (2), Data Length
  // Change (6), PHY Update Complete (11).
  // Classic event mask: Disconnection Complete (bit 4), Encryption Change
  // (bit 7), LE Meta (bit 61).
  sdc_hci_cmd_cb_set_event_mask_t evtmask = {};
  evtmask.raw[0] = 0x10 | 0x80;
  evtmask.raw[7] = 0x20;
  if((status = sdc_hci_cmd_cb_set_event_mask(&evtmask)))
    panic("sdc: event_mask: 0x%x", status);

  // LE event mask: Connection Complete (0), Connection Update (2), LTK
  // Request (4), Data Length Change (6), PHY Update Complete (11).
  sdc_hci_cmd_le_set_event_mask_t le_evtmask = {};
  le_evtmask.raw[0] = 0x45 | 0x10;
  le_evtmask.raw[1] = 0x08;
  if((status = sdc_hci_cmd_le_set_event_mask(&le_evtmask)))
    panic("sdc: le_event_mask: 0x%x", status);

  sdc_hci_cmd_le_read_buffer_size_return_t bufsz;
  if((status = sdc_hci_cmd_le_read_buffer_size(&bufsz)))
    panic("sdc: read_buffer_size: 0x%x", status);
  sb->sb_tx_ceiling = bufsz.total_num_le_acl_data_packets;
  sb->sb_tx_credits = sb->sb_tx_ceiling;

  // Static random address from the factory device address, so the device
  // identity is stable (SoC layer reads the right FICR location).
  nrf_ficr_ble_addr(sb->sb_addr);

  sdc_hci_cmd_le_set_random_address_t addr;
  memcpy(addr.random_address, sb->sb_addr, 6);
  if((status = sdc_hci_cmd_le_set_random_address(&addr)))
    panic("sdc: set_random_address: 0x%x", status);

  const sdc_hci_cmd_le_set_adv_params_t advp = {
    .adv_interval_min = 0xa0, // 100 ms
    .adv_interval_max = 0xa0,
    .adv_type = 0,            // ADV_IND
    .own_address_type = 1,    // random
    .adv_channel_map = 0x7,
  };
  if((status = sdc_hci_cmd_le_set_adv_params(&advp)))
    panic("sdc: set_adv_params: 0x%x", status);

  // Flags AD (mandatory for BlueZ discovery) + Complete Local Name.
  sdc_hci_cmd_le_set_adv_data_t advd = {};
  size_t namelen = strlen(sb->sb_name);
  if(namelen > 31 - 3 - 2)
    namelen = 31 - 3 - 2;
  uint8_t *p = advd.adv_data;
  p[0] = 2;
  p[1] = 1;   // Flags
  p[2] = 6;   // LE General Discoverable, BR/EDR not supported
  p[3] = namelen + 1;
  p[4] = 9;   // Complete Local Name
  memcpy(p + 5, sb->sb_name, namelen);
  advd.adv_data_length = 3 + 2 + namelen;
  if((status = sdc_hci_cmd_le_set_adv_data(&advd)))
    panic("sdc: set_adv_data: 0x%x", status);

  if((status = sdc_adv_enable(1)))
    panic("sdc: adv_enable: 0x%x", status);
}


// Runs on the main thread with interrupts enabled (after multitasking
// start): MPSL's LFCLK startup and the controller bring-up happen here.
static void __attribute__((constructor(5200)))
nrf_sdc_init(void)
{
  sdc_ble_t *sb = &g_sdc;

  if(sb->sb_name == NULL)
    return; // board did not enable BLE

  nrf_trng_init();
  nrf_mpsl_init(sdc_low_prio);

  // From here on the MPSL low priority interrupt may run; serialize all
  // controller API calls by blocking NET.
  int q = irq_forbid(IRQ_LEVEL_NET);

  int err = sdc_init(sdc_fault);
  if(err)
    panic("sdc_init: %d", err);

  sdc_support_adv();
  sdc_support_peripheral();
  sdc_support_dle_peripheral();
  sdc_support_le_2m_phy();
  sdc_support_phy_update_peripheral();

  // The default resource config assumes one central link too; claim only
  // what this glue implements so the memory pool stays minimal.
  const sdc_cfg_t central_cfg = { .central_count = { .count = 0 } };
  err = sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG,
                    SDC_CFG_TYPE_CENTRAL_COUNT, &central_cfg);
  if(err < 0)
    panic("sdc_cfg_set: %d", err);

  const sdc_cfg_t bufcfg = {
    .buffer_cfg = {
      .tx_packet_size = SDC_PKT_SIZE,
      .rx_packet_size = SDC_PKT_SIZE,
      .tx_packet_count = SDC_TX_COUNT,
      .rx_packet_count = SDC_RX_COUNT,
    },
  };
  err = sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG,
                    SDC_CFG_TYPE_BUFFER_CFG, &bufcfg);
  if(err < 0)
    panic("sdc_cfg_set: %d", err);

  err = sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG, SDC_CFG_TYPE_NONE, NULL);
  if(err < 0)
    panic("sdc_cfg_set: %d", err);
  if(err > (int)sizeof(sdc_mem))
    panic("sdc: pool too small: %d > %d", err, sizeof(sdc_mem));

  static const sdc_rand_source_t rand_source = {
    .rand_poll = sdc_rand_poll,
  };
  err = sdc_rand_source_register(&rand_source);
  if(err)
    panic("sdc_rand_source_register: %d", err);

  err = sdc_enable(sdc_hci_signal, sdc_mem);
  if(err)
    panic("sdc_enable: %d", err);

  sdc_setup_hci(sb);

  irq_permit(q);

  netif_init(&sb->sb_ni, "radio", &sdc_device_class);
  netif_attach(&sb->sb_ni);

  uint8_t rev[SDC_BUILD_REVISION_SIZE];
  sdc_build_revision_get(rev);
  printf("BLE: SoftDevice Controller %02x%02x%02x%02x "
         "(%02x:%02x:%02x:%02x:%02x:%02x)\n",
         rev[0], rev[1], rev[2], rev[3],
         sb->sb_addr[5], sb->sb_addr[4], sb->sb_addr[3],
         sb->sb_addr[2], sb->sb_addr[1], sb->sb_addr[0]);
}


#include <mios/cli.h>

// Ask the connected central to start pairing. Needed to test SMP with centrals
// (e.g. iOS) that never initiate pairing on their own.
static error_t
cmd_blepair(cli_t *cli, int argc, char **argv)
{
  sdc_ble_t *sb = &g_sdc;
  if(!sb->con.connected) {
    cli_printf(cli, "Not connected\n");
    return ERR_NOT_CONNECTED;
  }
  smp_request_security(&sb->con.l2c);
  cli_printf(cli, "Sent SMP Security Request\n");
  return 0;
}

CLI_CMD_DEF_EXT("ble_pair", cmd_blepair, NULL,
                "Ask the connected central to start pairing");

// Confirm the LE Secure Connections numeric-comparison value shown at pairing.
static error_t
cmd_bleconfirm(cli_t *cli, int argc, char **argv)
{
  sdc_ble_t *sb = &g_sdc;
  if(!sb->con.connected) {
    cli_printf(cli, "Not connected\n");
    return ERR_NOT_CONNECTED;
  }
  error_t err = smp_numeric_confirm(&sb->con.l2c);
  cli_printf(cli, err ? "No pairing waiting for confirmation\n" : "Confirmed\n");
  return err;
}

CLI_CMD_DEF_EXT("ble_confirm", cmd_bleconfirm, NULL,
                "Confirm the LE Secure Connections pairing value");
