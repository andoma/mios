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
#include "nrf_sdc_internal.h"

#include "net/pbuf.h"
#include "net/netif.h"
#include "net/ble/l2cap.h"
#include "net/ble/smp.h"

#include <malloc.h>
#include <mios/mios.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "irq.h"

#include "mpsl.h"
#include "sdc.h"
#include "sdc_soc.h"
#include "sdc_hci.h"
#include "sdc_hci_cmd_le.h"
#include "sdc_hci_cmd_controller_baseband.h"
#include "sdc_hci_vs.h"
#include "sdc_hci_evt.h"

// Link layer packet size: matched to our pbufs so an ACL fragment always
// fits one pbuf (the SDC negotiates DLE up to this).
#define SDC_PKT_SIZE LLMTU
#define SDC_TX_COUNT 3
#define SDC_RX_COUNT 2

// Controller memory pool. Allocated dynamically at init from the exact size
// sdc_cfg_set() reports for the configured features, so there is no
// compile-time worst-case sizing and no per-feature pool #ifdefs.
static void *sdc_mem;
static size_t sdc_mem_size;

// The sdc_ble_t definition lives in nrf_sdc_internal.h (shared with the
// optional Channel Sounding extension).
sdc_ble_t g_sdc;

#define SDC_TRY(step, expr)                                     \
  do {                                                          \
    int _s = (expr);                                            \
    if(_s) { g_sdc.sb_err_step = (step); g_sdc.sb_err_status = _s; return; } \
  } while(0)

// Channel Sounding hooks: weak no-ops here, overridden by nrf_sdc_cs.c when the
// build sets ENABLE_BLE_CS. This keeps all CS code -- and the controller's CS
// implementation, dropped by --gc-sections when no sdc_support_channel_sounding_*
// call remains -- out of builds that do not use it.
__attribute__((weak)) int  nrf_sdc_cs_configure(void) { return 0; }
__attribute__((weak)) void nrf_sdc_cs_setup_hci(void) {}
__attribute__((weak)) void nrf_sdc_cs_connected(sdc_ble_t *sb) { (void)sb; }
__attribute__((weak)) int  nrf_sdc_cs_le_meta(sdc_ble_t *sb, const uint8_t *p,
                                              uint8_t plen)
{ (void)sb; (void)p; (void)plen; return 0; }
__attribute__((weak)) int  nrf_sdc_cs_ltk(sdc_ble_t *sb) { (void)sb; return 0; }
__attribute__((weak)) int  nrf_sdc_cs_encryption_changed(sdc_ble_t *sb, int on)
{ (void)sb; (void)on; return 0; }

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
void
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


uint8_t
sdc_adv_enable(uint8_t on)
{
  uint8_t buf[sizeof(sdc_hci_cmd_le_set_ext_adv_enable_t) +
              sizeof(sdc_hci_le_set_ext_adv_enable_array_params_t)] = {0};
  sdc_hci_cmd_le_set_ext_adv_enable_t *en = (void *)buf;
  en->enable = on;
  en->num_sets = 1;
  en->array_params[0].adv_handle = 0;
  uint8_t status = sdc_hci_cmd_le_set_ext_adv_enable(en);
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

// Common connection-up path for both LE Connection Complete and LE Enhanced
// Connection Complete (extended advertising/scanning reports the latter).
static void
sdc_connected(sdc_ble_t *sb, uint8_t status, uint16_t handle, uint8_t role,
              uint8_t peer_addr_type, const uint8_t *peer_addr,
              uint16_t interval, uint16_t timeout)
{
  if(status)
    return;

  sb->con.handle = handle;
  sb->con.interval = interval;
  sb->con.timeout = timeout;
  sb->con.tx_phy = 1;
  sb->con.rx_phy = 1;
  sb->con.encrypted = 0;
  sb->con.role = role;
  memcpy(sb->con.peer, peer_addr, 6);

  // The l2cap/CoC host runs on the peripheral (CoC server) side. A central
  // link (e.g. a Channel Sounding initiator) just tracks the ACL connection.
  if(role == 1) {
    sb->con.l2c.l2c_output = sdc_conn_output;
    sb->con.l2c.l2c_ltk_reply = sdc_ltk_reply;
    STAILQ_INIT(&sb->con.l2c.l2c_tx_queue);
    sb->con.l2c.l2c_tx_queue_len = 0;

    // Addresses for the pairing crypto (HCI order = LSB first).
    memcpy(sb->con.l2c.l2c_peer_addr, peer_addr, 6);
    sb->con.l2c.l2c_peer_addr_type = peer_addr_type;
    memcpy(sb->con.l2c.l2c_our_addr, sb->sb_addr, 6);
    sb->con.l2c.l2c_our_addr_type = 1; // static random

    if(l2cap_connect(&sb->con.l2c))
      return;
  }

  sb->con.connected = 1;
  sb->sb_connecting = 0;
  sb->sb_advertising = 0; // controller stops advertising on connect

  // Optional Channel Sounding: as a peripheral this arms the CS reflector role
  // so a CS-capable central can drive a procedure right after pairing. No-op
  // unless the build enables CS.
  nrf_sdc_cs_connected(sb);

  netlog("ble: %s %02x:%02x:%02x:%02x:%02x:%02x interval:%d",
         role ? "Connected to" : "Connected (central) to",
         peer_addr[5], peer_addr[4], peer_addr[3],
         peer_addr[2], peer_addr[1], peer_addr[0], interval);
}

static void
sdc_handle_conn_complete(sdc_ble_t *sb, const uint8_t *p)
{
  const sdc_hci_subevent_le_conn_complete_t *cc = (const void *)p;
  sdc_connected(sb, cc->status, cc->conn_handle, cc->role,
                cc->peer_address_type, cc->peer_address,
                cc->conn_interval, cc->supervision_timeout);
}

static void
sdc_handle_enh_conn_complete(sdc_ble_t *sb, const uint8_t *p)
{
  const sdc_hci_subevent_le_enhanced_conn_complete_t *cc = (const void *)p;
  sdc_connected(sb, cc->status, cc->conn_handle, cc->role,
                cc->peer_address_type, cc->peer_address,
                cc->conn_interval, cc->supervision_timeout);
}


static void
sdc_handle_disconn(sdc_ble_t *sb, const uint8_t *p)
{
  const sdc_hci_event_disconn_complete_t *dc = (const void *)p;

  if(!sb->con.connected || dc->conn_handle != sb->con.handle)
    return;

  netlog("ble: Disconnected (reason=0x%x)", dc->reason);

  sb->con.connected = 0;
  sb->sb_connecting = 0;
  if(sb->con.role == 1) {
    l2cap_disconnect(&sb->con.l2c);
    pbuf_free_queue_irq_blocked(&sb->con.l2c.l2c_tx_queue);
    sb->con.l2c.l2c_tx_queue_len = 0;
  }
  sb->sb_tx_credits = sb->sb_tx_ceiling;

  sdc_adv_enable(1);
}


// --- Central role: scan for a peer by name, then connect ------------------

static uint8_t
sdc_start_scan(void)
{
  uint8_t buf[sizeof(sdc_hci_cmd_le_set_ext_scan_params_t) +
              sizeof(sdc_hci_le_set_ext_scan_params_array_params_t)] = {0};
  sdc_hci_cmd_le_set_ext_scan_params_t *sp = (void *)buf;
  sp->own_address_type = 1;          // random
  sp->scanning_phys = 0x01;          // LE 1M
  sp->array_params[0].scan_type = 0; // passive
  sp->array_params[0].scan_interval = 0x0060;
  sp->array_params[0].scan_window = 0x0030;
  uint8_t status = sdc_hci_cmd_le_set_ext_scan_params(sp);
  if(status)
    return status;

  sdc_hci_cmd_le_set_ext_scan_enable_t en = { .enable = 1 };
  status = sdc_hci_cmd_le_set_ext_scan_enable(&en);
  if(!status)
    g_sdc.sb_scanning = 1;
  return status;
}

static void
sdc_stop_scan(void)
{
  sdc_hci_cmd_le_set_ext_scan_enable_t en = { .enable = 0 };
  sdc_hci_cmd_le_set_ext_scan_enable(&en);
  g_sdc.sb_scanning = 0;
}

static uint8_t
sdc_create_conn(uint8_t peer_addr_type, const uint8_t *peer_addr)
{
  uint8_t buf[sizeof(sdc_hci_cmd_le_ext_create_conn_t) +
              sizeof(sdc_hci_le_ext_create_conn_array_params_t)] = {0};
  sdc_hci_cmd_le_ext_create_conn_t *cc = (void *)buf;
  cc->own_address_type = 1;    // random
  cc->peer_address_type = peer_addr_type;
  memcpy(cc->peer_address, peer_addr, 6);
  cc->initiating_phys = 0x01;  // LE 1M
  cc->array_params[0].scan_interval = 0x0060;
  cc->array_params[0].scan_window = 0x0030;
  cc->array_params[0].conn_interval_min = 0x0028; // 50 ms
  cc->array_params[0].conn_interval_max = 0x0028; // 50 ms
  cc->array_params[0].supervision_timeout = 0x0100; // 2.56 s
  return sdc_hci_cmd_le_ext_create_conn(cc);
}

// Match the target name against an AD payload (0x08 short / 0x09 complete).
static int
adv_name_matches(const uint8_t *data, uint8_t len, const char *prefix)
{
  const size_t plen = strlen(prefix);
  for(uint8_t i = 0; i + 1 < len; ) {
    uint8_t adlen = data[i];
    if(adlen == 0 || i + 1 + adlen > len)
      break;
    uint8_t adtype = data[i + 1];
    if((adtype == 0x08 || adtype == 0x09) && (size_t)(adlen - 1) >= plen &&
       !memcmp(&data[i + 2], prefix, plen))
      return 1;
    i += 1 + adlen;
  }
  return 0;
}

static void
sdc_handle_ext_adv_report(sdc_ble_t *sb, const uint8_t *p)
{
  if(!sb->sb_scanning || sb->sb_connecting)
    return;
  const sdc_hci_subevent_le_ext_adv_report_t *rep = (const void *)p;
  if(rep->num_reports < 1)
    return;
  const sdc_hci_le_ext_adv_report_array_params_t *r =
    (const void *)rep->reports;
  if(!adv_name_matches(r->data, r->data_length, sb->sb_target))
    return;

  const uint8_t addr_type = r->address_type;
  uint8_t addr[6];
  memcpy(addr, r->address, 6);
  sdc_stop_scan();
  if(sdc_create_conn(addr_type, addr) == 0)
    sb->sb_connecting = 1;
}

// Public: start acting as a central and connect to a peer whose advertised
// name begins with `prefix`. Safe to call from a thread (blocks NET).
int
nrf_ble_connect(const char *prefix)
{
  sdc_ble_t *sb = &g_sdc;
  int q = irq_forbid(IRQ_LEVEL_NET);
  int r = -1;
  if(!sb->con.connected && !sb->sb_connecting) {
    snprintf(sb->sb_target, sizeof(sb->sb_target), "%s", prefix);
    r = sdc_start_scan() ? -1 : 0;
  }
  irq_permit(q);
  return r;
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
    case SDC_HCI_SUBEVENT_LE_ENHANCED_CONN_COMPLETE:
      sdc_handle_enh_conn_complete(sb, p + 1);
      break;
    case SDC_HCI_SUBEVENT_LE_EXT_ADV_REPORT:
      sdc_handle_ext_adv_report(sb, p + 1);
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
      if(!nrf_sdc_cs_ltk(sb)) // CS (if built) may answer with its fixed test key
        smp_ltk_request(&sb->con.l2c, ltk->random_number,
                        ltk->encrypted_diversifier);
      break;
    }

    default:
      // The optional CS extension handles its LE subevents (and the LL
      // feature-exchange-complete it chains on); otherwise log as unhandled.
      if(!nrf_sdc_cs_le_meta(sb, p, buf[1]))
        netlog("le: unhandled subevent 0x%x", p[0]);
      break;
    }
    break;

  case SDC_HCI_EVENT_ENCRYPTION_CHANGE: {
    const sdc_hci_event_encryption_change_t *ec = (const void *)p;
    const int on = !ec->status && ec->encryption_enabled;
    sb->con.encrypted = on;
    // The optional CS extension consumes this on a fixed-key link (and chains
    // its next step); otherwise it belongs to SMP.
    if(!nrf_sdc_cs_encryption_changed(sb, on))
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

  if(sb->sb_err_step)
    stprintf(st, "  BRING-UP FAILED at %s (status %d / 0x%x)\n",
             sb->sb_err_step, sb->sb_err_status, sb->sb_err_status);

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

  if(sb->cs.active || sb->cs.procedures || sb->cs.step)
    stprintf(st, "  CS: active:%d  procedures:%d  last_steps:%d  last:%s(0x%x)\n",
             sb->cs.active, sb->cs.procedures, sb->cs.last_steps,
             sb->cs.step ? sb->cs.step : "-", sb->cs.status);
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
  // Classic event mask: Disconnection Complete (bit 4), Encryption Change
  // (bit 7), LE Meta (bit 61).
  sdc_hci_cmd_cb_set_event_mask_t evtmask = {};
  evtmask.raw[0] = 0x10 | 0x80;
  evtmask.raw[7] = 0x20;
  SDC_TRY("event_mask", sdc_hci_cmd_cb_set_event_mask(&evtmask));

  // LE event mask. Byte 0: Connection Complete (0), Connection Update (2),
  // LTK Request (4), Data Length Change (6). Byte 1: Enhanced Connection
  // Complete (9), PHY Update Complete (11), Extended Advertising Report (12).
  // With extended advertising/scanning the controller reports connections via
  // Enhanced Connection Complete, and scan results via Extended Adv Report.
  sdc_hci_cmd_le_set_event_mask_t le_evtmask = {};
  le_evtmask.raw[0] = 0x5d; // bits 0,2,3,4,6 (3 = read remote features complete)
  le_evtmask.raw[1] = 0x1a; // bits 9,11,12
  SDC_TRY("le_event_mask", sdc_hci_cmd_le_set_event_mask(&le_evtmask));

  // Optional Channel Sounding HCI setup: CS host-support feature bit, CS
  // subevent mask bits, and the ACL event-length reservation CS needs. No-op
  // unless the build enables CS.
  nrf_sdc_cs_setup_hci();

  sdc_hci_cmd_le_read_buffer_size_return_t bufsz;
  SDC_TRY("read_buffer_size", sdc_hci_cmd_le_read_buffer_size(&bufsz));
  sb->sb_tx_ceiling = bufsz.total_num_le_acl_data_packets;
  sb->sb_tx_credits = sb->sb_tx_ceiling;

  // Static random address from the factory device address, so the device
  // identity is stable (SoC layer reads the right FICR location).
  nrf_ficr_ble_addr(sb->sb_addr);

  // Global random address is used when scanning / initiating (central role).
  sdc_hci_cmd_le_set_random_address_t addr;
  memcpy(addr.random_address, sb->sb_addr, 6);
  SDC_TRY("set_random_address", sdc_hci_cmd_le_set_random_address(&addr));

  // Extended advertising, but emitting legacy ADV_IND PDUs (connectable +
  // scannable + legacy): the modern command set, still discoverable by hosts
  // (e.g. iOS) that don't scan extended-only advertisements.
  sdc_hci_cmd_le_set_ext_adv_params_t advp = {
    .adv_handle = 0,
    .primary_adv_interval_min = 0xa0, // 100 ms (0.625 ms units)
    .primary_adv_interval_max = 0xa0,
    .primary_adv_channel_map = 0x7,
    .own_address_type = 1,            // random
    .adv_tx_power = 0x7f,             // no preference
    .primary_adv_phy = 1,            // LE 1M
    .secondary_adv_phy = 1,          // LE 1M
    .adv_sid = 0,
  };
  advp.adv_event_properties.params.connectable_adv = 1;
  advp.adv_event_properties.params.scannable_adv = 1;
  advp.adv_event_properties.params.legacy_adv_packets = 1;
  sdc_hci_cmd_le_set_ext_adv_params_return_t advp_ret;
  SDC_TRY("set_ext_adv_params",
          sdc_hci_cmd_le_set_ext_adv_params(&advp, &advp_ret));

  // The advertising set now exists (handle 0); give it its random address.
  sdc_hci_cmd_le_set_adv_set_random_address_t setaddr = { .adv_handle = 0 };
  memcpy(setaddr.random_address, sb->sb_addr, 6);
  SDC_TRY("set_adv_set_random_address",
          sdc_hci_cmd_le_set_adv_set_random_address(&setaddr));

  // Flags AD (mandatory for discovery) + Complete Local Name.
  uint8_t dbuf[sizeof(sdc_hci_cmd_le_set_ext_adv_data_t) + 31] = {0};
  sdc_hci_cmd_le_set_ext_adv_data_t *advd = (void *)dbuf;
  advd->adv_handle = 0;
  advd->operation = 0x03;         // complete adv data
  advd->fragment_preference = 1;  // controller should not fragment
  size_t namelen = strlen(sb->sb_name);
  if(namelen > 31 - 3 - 2)
    namelen = 31 - 3 - 2;
  uint8_t *p = advd->adv_data;
  p[0] = 2;
  p[1] = 1;   // Flags
  p[2] = 6;   // LE General Discoverable, BR/EDR not supported
  p[3] = namelen + 1;
  p[4] = 9;   // Complete Local Name
  memcpy(p + 5, sb->sb_name, namelen);
  advd->adv_data_length = 3 + 2 + namelen;
  SDC_TRY("set_ext_adv_data", sdc_hci_cmd_le_set_ext_adv_data(advd));

  SDC_TRY("adv_enable", sdc_adv_enable(1));
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
  if(err) { sb->sb_err_step = "sdc_init"; sb->sb_err_status = err; goto done; }

  sdc_support_ext_adv();
  sdc_support_ext_scan();
  sdc_support_peripheral();
  sdc_support_central();
  sdc_support_dle_central();
  sdc_support_dle_peripheral();
  sdc_support_le_2m_phy();
  sdc_support_phy_update_central();
  sdc_support_phy_update_peripheral();

  // Optional Channel Sounding: enables the CS roles + LE Power Control and the
  // CS resource cfg. Returns 0 (no-op) unless the build enables CS. Must run
  // after the base sdc_support_*() and before the cfg below.
  err = nrf_sdc_cs_configure();
  if(err < 0) { sb->sb_err_step = "cfg_cs"; sb->sb_err_status = err; goto done; }

  const sdc_cfg_t central_cfg = { .central_count = { .count = 1 } };
  err = sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG,
                    SDC_CFG_TYPE_CENTRAL_COUNT, &central_cfg);
  if(err < 0) { sb->sb_err_step = "cfg_central"; sb->sb_err_status = err; goto done; }

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
  if(err < 0) { sb->sb_err_step = "cfg_buffer"; sb->sb_err_status = err; goto done; }

  // Ask the controller how much memory this configuration needs, then allocate
  // exactly that (8-byte aligned). No compile-time worst-case pool sizing.
  err = sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG, SDC_CFG_TYPE_NONE, NULL);
  if(err < 0) { sb->sb_err_step = "cfg_none"; sb->sb_err_status = err; goto done; }
  sdc_mem_size = err;
  sdc_mem = xalloc(sdc_mem_size, 8, MEM_MAY_FAIL);
  if(sdc_mem == NULL) {
    sb->sb_err_step = "pool_alloc"; sb->sb_err_status = (int)sdc_mem_size;
    goto done;
  }

  static const sdc_rand_source_t rand_source = {
    .rand_poll = sdc_rand_poll,
  };
  err = sdc_rand_source_register(&rand_source);
  if(err) { sb->sb_err_step = "rand_source"; sb->sb_err_status = err; goto done; }

  err = sdc_enable(sdc_hci_signal, sdc_mem);
  if(err) { sb->sb_err_step = "sdc_enable"; sb->sb_err_status = err; goto done; }

  sdc_setup_hci(sb); // records sb_err_step on failure, but keeps booting

 done:
  irq_permit(q);

  netif_init(&sb->sb_ni, "radio", &sdc_device_class);
  netif_attach(&sb->sb_ni);

  if(sb->sb_err_step) {
    printf("BLE: bring-up FAILED at %s (status %d / 0x%x)\n",
           sb->sb_err_step, sb->sb_err_status, sb->sb_err_status);
    return;
  }

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

// Act as central: scan for a peer advertising the given name prefix (default
// "mios") and connect to it.
static error_t
cmd_bleconnect(cli_t *cli, int argc, char **argv)
{
  const char *prefix = argc > 1 ? argv[1] : "mios";
  int r = nrf_ble_connect(prefix);
  cli_printf(cli, r ? "Busy or already connected\n" : "Scanning for '%s'\n",
             prefix);
  return r ? ERR_NOT_READY : 0;
}

CLI_CMD_DEF_EXT("ble_connect", cmd_bleconnect, NULL,
                "Scan and connect to a peer [name-prefix]");

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
