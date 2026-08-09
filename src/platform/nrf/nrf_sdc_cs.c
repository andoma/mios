// Optional Channel Sounding extension for the Nordic SoftDevice Controller.
// Built only when ENABLE_BLE_CS is set (see nrf.mk); it provides strong
// overrides for the CS hooks that are weak no-ops in nrf_sdc.c, plus the CS
// CLI commands. When not built, no sdc_support_channel_sounding_*() call
// remains, so --gc-sections drops the controller's CS implementation too.
//
// CS on nRF54L needs (all found by matching Nordic's connected_cs sample):
//   - an encrypted link (here via a fixed test LTK, so two mios devices can
//     do CS without implementing SMP pairing between themselves);
//   - a limited channel map (few steps per procedure); and, crucially,
//   - a subevent that FITS within one connection interval -- this port's SDC
//     integration cannot preempt ACL anchors for an interval-spanning
//     subevent, which aborts with subevent_abort_reason 0x3 ("scheduling /
//     limited resources"). 50 ms interval + 5 ms subevent works.

#include "nrf_sdc_internal.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include <unistd.h>

#include <mios/cli.h>
#include <mios/error.h>
#include <mios/task.h>
#include <mios/fs.h>

#include "net/pbuf.h"
#include "net/ble/l2cap_proto.h"

#include "irq.h"

#include "sdc.h"
#include "sdc_hci.h"
#include "sdc_hci_cmd_le.h"
#include "sdc_hci_vs.h"
#include "sdc_hci_evt.h"

// Fixed test LTK for device-to-device CS. Both DKs run the same firmware and
// share this key, so the link can be encrypted (a CS prerequisite) without
// implementing SMP pairing between two mios devices. Test-only.
static const uint8_t cs_fixed_ltk[16] = {
  0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
  0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
};

// --- CS procedure steps (initiator drives; events chain the next step) ------

static void
cs_security_enable(sdc_ble_t *sb)
{
  sdc_hci_cmd_le_cs_security_enable_t se = { .conn_handle = sb->con.handle };
  uint8_t s = sdc_hci_cmd_le_cs_security_enable(&se);
  sb->cs.step = "security_enable";
  sb->cs.status = s;
  if(s)
    netlog("cs: security_enable failed 0x%x", s);
}

static void
cs_read_features(sdc_ble_t *sb)
{
  sdc_hci_cmd_le_read_remote_features_t r = { .conn_handle = sb->con.handle };
  uint8_t s = sdc_hci_cmd_le_read_remote_features(&r);
  sb->cs.step = "read_features";
  sb->cs.status = s;
  if(s)
    netlog("cs: read_features failed 0x%x", s);
}

static void
cs_enable_encryption(sdc_ble_t *sb)
{
  sdc_hci_cmd_le_enable_encryption_t en = { .conn_handle = sb->con.handle };
  memcpy(en.long_term_key, cs_fixed_ltk, 16);
  uint8_t s = sdc_hci_cmd_le_enable_encryption(&en);
  sb->cs.step = "enable_encryption";
  sb->cs.status = s;
  if(s)
    netlog("cs: enable_encryption failed 0x%x", s);
}

static void
cs_read_remote_caps(sdc_ble_t *sb)
{
  sdc_hci_cmd_le_cs_read_remote_supported_capabilities_t r = {
    .conn_handle = sb->con.handle,
  };
  uint8_t s = sdc_hci_cmd_le_cs_read_remote_supported_capabilities(&r);
  sb->cs.step = "read_remote_caps";
  sb->cs.status = s;
  if(s)
    netlog("cs: read_remote_caps failed 0x%x", s);
}

static void
cs_create_config(sdc_ble_t *sb)
{
  sdc_hci_cmd_le_cs_create_config_t cc = {
    .conn_handle = sb->con.handle,
    .config_id = 0,
    .create_context = 1,       // apply on both local and remote
    .main_mode_type = 2,       // mode-2 PBR (per CS sample)
    .sub_mode_type = 1,        // sub-mode-1 RTT interleaved (per CS sample)
    .min_main_mode_steps = 2,
    .max_main_mode_steps = 10,
    .main_mode_repetition = 0,
    .mode_0_steps = 1,
    .role = 0,                 // initiator
    .rtt_type = 0,             // AA-only RTT (mandatory, always supported)
    .cs_sync_phy = 1,          // LE 1M CS_SYNC (per CS sample)
    .channel_map_repetition = 1,
    .channel_selection_type = 0, // algorithm #3b
  };
  // 18 channels, every other one across 26..60 (2428..2462 MHz). A wide span
  // for the phase-slope fit but few enough tones that the whole averaged set
  // fits ONE L2CAP frame (nRF54L PBUF_DATA_SIZE is 128): the reflector never
  // sends a multi-frame burst, avoiding rx-queue churn on the initiator.
  // bit (i & 7) of byte (i / 8) = channel i.
  static const uint8_t cs_chmap[10] = {
    0x00, 0x00, 0x00, // ch 0-23
    0x54,             // ch 24-31: 26,28,30
    0x55,             // ch 32-39: 32,34,36,38
    0x55,             // ch 40-47: 40,42,44,46
    0x55,             // ch 48-55: 48,50,52,54
    0x15,             // ch 56-63: 56,58,60
    0x00, 0x00,       // ch 64-79
  };
  memcpy(cc.channel_map, cs_chmap, sizeof(cc.channel_map));
  uint8_t s = sdc_hci_cmd_le_cs_create_config(&cc);
  sb->cs.step = "create_config";
  sb->cs.status = s;
  if(s)
    netlog("cs: create_config failed 0x%x", s);
}

static void
cs_start_procedure(sdc_ble_t *sb)
{
  sdc_hci_cmd_le_cs_set_procedure_params_t pp = {
    .conn_handle = sb->con.handle,
    .config_id = 0,
    // The subevent must FIT within one connection interval (ACL event + guard +
    // subevent < interval); this port cannot preempt ACL anchors for an
    // interval-spanning subevent (aborts 0x30). 50 ms interval + 5 ms subevent.
    .max_procedure_len = 0xffff,
    // ~0.5 s between procedures (10 x 50 ms interval): responsive enough to
    // track a reflector being moved around, still light on the scheduler.
    .min_procedure_interval = 10,   // connection events
    .max_procedure_interval = 10,
    .max_procedure_count = 0,       // 0 = repeat until disabled
    .min_subevent_len = 5000,       // us (fits the 50 ms interval)
    .max_subevent_len = 5000,
    .tone_antenna_config_selection = 0, // A1:B1 (single path)
    .phy = 2,                       // LE 2M for the mode payload (per sample)
    .tx_power_delta = 0x80,         // 0x80 = no recommendation (per sample)
    .preferred_peer_antenna = 1,
    .snr_control_initiator = 0xff,  // no SNR control
    .snr_control_reflector = 0xff,
  };
  sdc_hci_cmd_le_cs_set_procedure_params_return_t ppr;
  uint8_t s = sdc_hci_cmd_le_cs_set_procedure_params(&pp, &ppr);
  sb->cs.step = "set_procedure_params";
  sb->cs.status = s;
  if(s) { netlog("cs: set_procedure_params failed 0x%x", s); return; }

  sdc_hci_cmd_le_cs_procedure_enable_t pe = {
    .conn_handle = sb->con.handle, .config_id = 0, .enable = 1,
  };
  s = sdc_hci_cmd_le_cs_procedure_enable(&pe);
  sb->cs.step = "procedure_enable";
  sb->cs.status = s;
  if(s)
    netlog("cs: procedure_enable failed 0x%x", s);
}


// --- Per-tone IQ capture (mode-2 PBR) ---------------------------------------
// The subevent result carries the step data: a sequence of steps, each
//   mode(1) channel(1) data_len(1) data[data_len].
// A mode-2 step's data is antenna_permutation_index(1) followed by (n_ap+1)
// tone_info records of 4 bytes: a 24-bit phase correction term (12-bit I in
// bits 0..11, 12-bit Q in bits 12..23, both two's complement) plus a
// quality/extension nibble byte. We keep the primary antenna path's I/Q per
// channel for the last subevent, for read-out via the `cs_data` command; the
// distance itself (phase slope vs frequency, combining this device's tones
// with the peer's) is computed off-device for now.
#define CS_MAX_TONES 80
// One accumulator per CS channel: a running sum of the (good-quality) tone I/Q
// across all subevents since the last cs_start. Averaging many subevents over
// the full channel set (rather than one short subevent's handful of tones)
// gives a much steadier phase-slope fit.
struct cs_tone {
  uint8_t channel;
  uint16_t count;
  int32_t isum;
  int32_t qsum;
};
static struct cs_tone cs_tones[CS_MAX_TONES];
static uint8_t cs_ntones;

static void
cs_store_tone(uint8_t channel, const uint8_t *tone)
{
  if((tone[3] & 0x0f) != 0)
    return; // quality_indicator != good -> drop
  uint32_t pct = tone[0] | (tone[1] << 8) | (tone[2] << 16);
  int16_t i = (int16_t)((pct & 0x000fff) ^ 0x800) - 0x800;
  int16_t q = (int16_t)(((pct & 0xfff000) >> 12) ^ 0x800) - 0x800;

  struct cs_tone *t = NULL;
  for(uint8_t k = 0; k < cs_ntones; k++)
    if(cs_tones[k].channel == channel) { t = &cs_tones[k]; break; }
  if(t == NULL) {
    if(cs_ntones >= CS_MAX_TONES)
      return;
    t = &cs_tones[cs_ntones++];
    t->channel = channel;
    t->count = 0;
    t->isum = 0;
    t->qsum = 0;
  }
  t->isum += i;
  t->qsum += q;
  t->count++;
}

// Walk a step-data blob, capturing the primary tone of each mode-2 step.
static void
cs_parse_steps(const uint8_t *d, int len)
{
  int off = 0;
  while(off + 3 <= len) {
    uint8_t mode = d[off];
    uint8_t channel = d[off + 1];
    uint8_t dlen = d[off + 2];
    off += 3;
    if(off + dlen > len)
      break;
    // mode-2: antenna_permutation_index(1) + tone_info[] (4 bytes each). Use
    // the first tone (primary antenna path) for single-antenna ranging.
    if(mode == 2 && dlen >= 1 + 4)
      cs_store_tone(channel, d + off + 1);
    off += dlen;
  }
}

// --- Live on-device ranging -------------------------------------------------
// The reflector ships its averaged per-channel tones to the initiator over the
// private L2CAP CS channel; the initiator combines them with its own tones and
// reports the distance (phase slope vs frequency) live on its console. Float
// math must run in a thread that owns the FPU (mios enables the FPU per task),
// so the L2CAP RX path only snapshots integers and a worker thread computes.

struct cs_iq { uint8_t channel; int16_t i; int16_t q; };

// Double-buffered snapshot handed from the NET-level RX path to the worker.
static struct cs_iq cs_snap_own[CS_MAX_TONES];
static struct cs_iq cs_snap_peer[CS_MAX_TONES];
static uint8_t cs_snap_own_n;
static uint8_t cs_snap_peer_n;
static volatile uint32_t cs_snap_gen; // bumped on RX; polled by the worker

static int cs_cal_cm;           // board/antenna offset subtracted from range
static uint8_t cs_auto_reflect; // headless reflector mode (persisted in fs)

// Reflector -> initiator over L2CAP_CID_CS. The full tone set does not fit one
// pbuf (nRF54L PBUF_DATA_SIZE is 128), so it is chunked. Each frame is
//   [flags:1][n:1] then n x (channel:1, i:int16 LE, q:int16 LE)
// flags bit0 = START (receiver resets its accumulation), bit1 = END (receiver
// publishes the set and bumps the generation). i/q are the window average.
#define CS_TONES_PER_FRAME 20 // 2 + 20*5 = 102 bytes payload, fits 128 pbuf

static void
cs_send_tones(sdc_ble_t *sb)
{
  uint8_t total = cs_ntones;
  if(total == 0)
    return;
  if(total > CS_MAX_TONES)
    total = CS_MAX_TONES;

  uint8_t sent = 0;
  while(sent < total) {
    uint8_t n = total - sent;
    if(n > CS_TONES_PER_FRAME)
      n = CS_TONES_PER_FRAME;
    pbuf_t *pb = pbuf_make(8, 0);
    if(pb == NULL)
      break;
    uint8_t flags = (sent == 0 ? 1 : 0) | (sent + n >= total ? 2 : 0);
    uint8_t *o = pbuf_append(pb, 2 + n * 5);
    o[0] = flags;
    o[1] = n;
    uint8_t *p = o + 2;
    for(uint8_t k = 0; k < n; k++) {
      struct cs_tone *t = &cs_tones[sent + k];
      int c = t->count ? t->count : 1;
      int16_t i = t->isum / c;
      int16_t q = t->qsum / c;
      *p++ = t->channel;
      *p++ = i & 0xff;
      *p++ = (i >> 8) & 0xff;
      *p++ = q & 0xff;
      *p++ = (q >> 8) & 0xff;
    }
    l2cap_output(&sb->con.l2c, pb, L2CAP_CID_CS);
    sent += n;
  }
  cs_ntones = 0;       // start the next averaging window
  board_cs_activity(); // a ranging update went out
}

// NET context: reassemble the chunked peer tone set. On END, snapshot it plus
// our own accumulator for the same window and bump the generation (integer
// only; the float ranging math runs later in the shell thread).
static struct cs_iq cs_peer_build[CS_MAX_TONES];
static uint8_t cs_peer_build_n;

void
ble_cs_l2cap_input(l2cap_t *l2c, pbuf_t *pb)
{
  (void)l2c;
  const uint8_t *d = pbuf_data(pb, 0);
  int len = pb->pb_pktlen;
  if(len < 2)
    return;
  uint8_t flags = d[0];
  uint8_t n = d[1];
  if(2 + (int)n * 5 > len)
    return;
  if(flags & 1)
    cs_peer_build_n = 0; // START: new set
  const uint8_t *p = d + 2;
  for(uint8_t k = 0; k < n && cs_peer_build_n < CS_MAX_TONES; k++) {
    cs_peer_build[cs_peer_build_n].channel = p[0];
    cs_peer_build[cs_peer_build_n].i = (int16_t)(p[1] | (p[2] << 8));
    cs_peer_build[cs_peer_build_n].q = (int16_t)(p[3] | (p[4] << 8));
    cs_peer_build_n++;
    p += 5;
  }
  if(!(flags & 2))
    return; // wait for END

  memcpy(cs_snap_peer, cs_peer_build, cs_peer_build_n * sizeof(cs_snap_peer[0]));
  cs_snap_peer_n = cs_peer_build_n;

  uint8_t m = cs_ntones;
  if(m > CS_MAX_TONES)
    m = CS_MAX_TONES;
  for(uint8_t k = 0; k < m; k++) {
    int c = cs_tones[k].count ? cs_tones[k].count : 1;
    cs_snap_own[k].channel = cs_tones[k].channel;
    cs_snap_own[k].i = cs_tones[k].isum / c;
    cs_snap_own[k].q = cs_tones[k].qsum / c;
  }
  cs_snap_own_n = m;
  cs_ntones = 0; // next window
  cs_snap_gen++;
}

// Phase-slope distance from the latest snapshot's common channels. Runs in the
// caller's thread (FPU-safe). Prints one line via cli and returns the tone
// count used, or -1 if too few common tones. Scratch is file-static to keep the
// stack frame under the 192-byte limit.
static struct cs_iq cs_w_own[CS_MAX_TONES];
static struct cs_iq cs_w_peer[CS_MAX_TONES];
static float cs_w_freq[CS_MAX_TONES];
static float cs_w_theta[CS_MAX_TONES];

static int
cs_range_once(cli_t *cli)
{
  int fl = irq_forbid(IRQ_LEVEL_NET);
  uint8_t on = cs_snap_own_n, pn = cs_snap_peer_n;
  memcpy(cs_w_own, cs_snap_own, on * sizeof(cs_w_own[0]));
  memcpy(cs_w_peer, cs_snap_peer, pn * sizeof(cs_w_peer[0]));
  irq_permit(fl);

  // combined = own x peer per common channel; theta vs frequency, sorted.
  int np = 0;
  for(uint8_t a = 0; a < on; a++) {
    for(uint8_t b = 0; b < pn; b++) {
      if(cs_w_own[a].channel != cs_w_peer[b].channel)
        continue;
      float ci = (float)cs_w_own[a].i * cs_w_peer[b].i -
                 (float)cs_w_own[a].q * cs_w_peer[b].q;
      float cq = (float)cs_w_own[a].i * cs_w_peer[b].q +
                 (float)cs_w_own[a].q * cs_w_peer[b].i;
      float fr = 2402.0f + cs_w_own[a].channel; // MHz
      float th = atan2f(cq, ci);
      int pos = np;
      while(pos > 0 && cs_w_freq[pos - 1] > fr) {
        cs_w_freq[pos] = cs_w_freq[pos - 1];
        cs_w_theta[pos] = cs_w_theta[pos - 1];
        pos--;
      }
      cs_w_freq[pos] = fr;
      cs_w_theta[pos] = th;
      np++;
      break;
    }
  }
  if(np < 4)
    return -1;

  for(int k = 1; k < np; k++) { // 1-D phase unwrap
    float dd = cs_w_theta[k] - cs_w_theta[k - 1];
    if(dd > M_PIf)
      for(int j = k; j < np; j++)
        cs_w_theta[j] -= 2 * M_PIf;
    else if(dd < -M_PIf)
      for(int j = k; j < np; j++)
        cs_w_theta[j] += 2 * M_PIf;
  }

  float fm = 0, tm = 0;
  for(int k = 0; k < np; k++) {
    fm += cs_w_freq[k];
    tm += cs_w_theta[k];
  }
  fm /= np;
  tm /= np;
  float num = 0, den = 0;
  for(int k = 0; k < np; k++) {
    num += (cs_w_freq[k] - fm) * (cs_w_theta[k] - tm);
    den += (cs_w_freq[k] - fm) * (cs_w_freq[k] - fm);
  }
  if(den == 0)
    return -1;
  float slope = num / den;                                   // rad/MHz
  float dist = -slope * (299792458.0f / (4 * M_PIf)) / 1e6f; // metres
  dist -= cs_cal_cm / 100.0f;
  int cm = (int)(dist * 100.0f + (dist >= 0 ? 0.5f : -0.5f));
  cli_printf(cli, "cs range: %d cm  (n=%d)\n", cm, np);
  return np;
}

__attribute__((weak)) void
board_cs_activity(void)
{
}

// --- Hooks called from nrf_sdc.c --------------------------------------------

int
nrf_sdc_cs_configure(void)
{
  // CS roles for both link roles (same firmware runs on both DKs). CS depends
  // on LE Power Control. Must be called after the base sdc_support_*().
  sdc_support_le_power_control_central();
  sdc_support_le_power_control_peripheral();
  sdc_support_channel_sounding_initiator_role_central();
  sdc_support_channel_sounding_initiator_role_peripheral();
  sdc_support_channel_sounding_reflector_role_central();
  sdc_support_channel_sounding_reflector_role_peripheral();
  sdc_support_channel_sounding_mode3();
  sdc_support_channel_sounding_test(); // for the standalone cs_test probe

  const sdc_cfg_t cs_count_cfg = { .cs_count = { .count = 1 } };
  int err = sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG,
                        SDC_CFG_TYPE_CS_COUNT, &cs_count_cfg);
  if(err < 0)
    return err;

  const sdc_cfg_t cs_cfg = {
    .cs_cfg = { .max_antenna_paths_supported = 1, .num_antennas_supported = 1 },
  };
  err = sdc_cfg_set(SDC_DEFAULT_RESOURCE_CFG_TAG, SDC_CFG_TYPE_CS_CFG, &cs_cfg);
  if(err < 0)
    return err;
  return 0;
}

void
nrf_sdc_cs_setup_hci(void)
{
  // CS is host-supported: the controller disallows all CS commands until the
  // host sets the CS Host Support feature bit (47).
  sdc_hci_cmd_le_set_host_feature_t hf = { .bit_number = 47, .bit_value = 1 };
  if(sdc_hci_cmd_le_set_host_feature(&hf)) {
    netlog("cs: set host feature failed");
    return;
  }

  // Re-set the LE event mask: the base bits plus the CS subevents (code C ->
  // bit C-1): read-remote-caps (0x2c/43), security-enable (0x2e/45), config
  // (0x2f/46), procedure-enable (0x30/47), subevent result (0x31/48) and
  // continue (0x32/49).
  sdc_hci_cmd_le_set_event_mask_t m = {};
  m.raw[0] = 0x5d; // base: bits 0,2,3,4,6
  m.raw[1] = 0x1a; // base: bits 9,11,12
  m.raw[5] = 0xe8; // CS: 43,45,46,47
  m.raw[6] = 0x03; // CS: 48,49
  sdc_hci_cmd_le_set_event_mask(&m);

  // Keep the ACL event reservation short so a CS subevent fits within the
  // connection interval (event + subevent must be < interval).
  const sdc_hci_cmd_vs_event_length_set_t evl = { .event_length_us = 2500 };
  sdc_hci_cmd_vs_event_length_set(&evl);
}

void
nrf_sdc_cs_connected(sdc_ble_t *sb)
{
  if(sb->con.role != 1)
    return; // only the ACL peripheral arms as reflector

  // Arm the CS reflector role so a CS-capable central can drive a procedure
  // right after pairing, without a local CLI command. Real SMP does the
  // required encryption (fixed_key stays 0); harmless for non-CS peers.
  const sdc_hci_cmd_le_cs_set_default_settings_t ds = {
    .conn_handle = sb->con.handle,
    .role_enable = 0x2, // reflector
    .cs_sync_antenna_selection = 1,
    .max_tx_power = 10,
  };
  sdc_hci_cmd_le_cs_set_default_settings_return_t dsr;
  if(sdc_hci_cmd_le_cs_set_default_settings(&ds, &dsr))
    netlog("cs: reflector set_default_settings failed");

  // Headless reflector (battery demo): no CLI available, so auto-arm the fixed
  // test LTK here and start a fresh tone window. Only in this persisted mode,
  // so normal SMP pairing is unaffected when it is off.
  if(cs_auto_reflect) {
    sb->cs.fixed_key = 1;
    cs_ntones = 0;
    netlog("cs: headless reflector armed");
  }
}

int
nrf_sdc_cs_le_meta(sdc_ble_t *sb, const uint8_t *p, uint8_t plen)
{
  switch(p[0]) {
  case 0x04: // LE Read Remote Features Complete
    // Feature exchange done; the CS chain can now proceed reliably.
    if(sb->cs.want_cs && sb->con.role == 0)
      cs_enable_encryption(sb);
    return 1;

  case SDC_HCI_SUBEVENT_LE_CS_READ_REMOTE_SUPPORTED_CAPABILITIES_COMPLETE: {
    const sdc_hci_subevent_le_cs_read_remote_supported_capabilities_complete_t
      *e = (const void *)(p + 1);
    netlog("cs: remote caps status=0x%x roles=0x%x modes=0x%x sync_phys=0x%x "
           "rtt_cap=0x%x n_ant=%d paths=%d",
           e->status, e->roles_supported, e->modes_supported,
           e->cs_sync_phys_supported, e->rtt_capability,
           e->num_antennas_supported, e->max_antenna_paths_supported);
    if(!e->status && sb->con.role == 0)
      cs_security_enable(sb);
    return 1;
  }
  case SDC_HCI_SUBEVENT_LE_CS_SECURITY_ENABLE_COMPLETE: {
    const sdc_hci_subevent_le_cs_security_enable_complete_t *e =
      (const void *)(p + 1);
    netlog("cs: security complete status=0x%x", e->status);
    if(!e->status && sb->con.role == 0)
      cs_create_config(sb);
    return 1;
  }
  case SDC_HCI_SUBEVENT_LE_CS_CONFIG_COMPLETE: {
    const sdc_hci_subevent_le_cs_config_complete_t *e = (const void *)(p + 1);
    netlog("cs: config complete status=0x%x action=%d mode=%d",
           e->status, e->action, e->main_mode_type);
    if(!e->status && e->action == 1 && sb->con.role == 0)
      cs_start_procedure(sb);
    return 1;
  }
  case SDC_HCI_SUBEVENT_LE_CS_PROCEDURE_ENABLE_COMPLETE: {
    const sdc_hci_subevent_le_cs_procedure_enable_complete_t *e =
      (const void *)(p + 1);
    netlog("cs: procedure enable status=0x%x state=%d", e->status, e->state);
    sb->cs.active = !e->status && e->state == 1;
    return 1;
  }
  case SDC_HCI_SUBEVENT_LE_CS_SUBEVENT_RESULT: {
    const sdc_hci_subevent_le_cs_subevent_result_t *e = (const void *)(p + 1);
    sb->cs.procedures++;
    sb->cs.last_steps = e->num_steps_reported;
    netlog("cs: result proc=%d steps=%d pdone=%d sdone=%d abort=0x%x paths=%d",
           e->procedure_counter, e->num_steps_reported,
           e->procedure_done_status, e->subevent_done_status,
           e->abort_reason, e->num_antenna_paths);
    // A new procedure's report begins here, so the reflector's accumulator now
    // holds the previous procedure's full averaged tone set: ship it to the
    // initiator before starting the next window.
    if(sb->con.role == 1 && cs_ntones > 0)
      cs_send_tones(sb);
    // Parse this report's steps into the per-channel tone accumulator (reset at
    // cs_start / each window, so it averages across the window's subevents).
    // data[] length = (LE-meta params) - subevent code - fixed struct fields.
    cs_parse_steps(e->data,
                   (int)plen - 1 -
                   (int)offsetof(sdc_hci_subevent_le_cs_subevent_result_t, data));
    return 1;
  }
  case SDC_HCI_SUBEVENT_LE_CS_SUBEVENT_RESULT_CONTINUE: {
    const sdc_hci_subevent_le_cs_subevent_result_continue_t *e =
      (const void *)(p + 1);
    // More steps for the same subevent: append to the tone capture.
    cs_parse_steps(e->data,
                   (int)plen - 1 -
                   (int)offsetof(sdc_hci_subevent_le_cs_subevent_result_continue_t,
                                 data));
    return 1;
  }

  default:
    return 0;
  }
}

int
nrf_sdc_cs_ltk(sdc_ble_t *sb)
{
  if(!sb->cs.fixed_key)
    return 0;
  sdc_ltk_reply(&sb->con.l2c, cs_fixed_ltk); // CS test: fixed shared key
  return 1;
}

int
nrf_sdc_cs_encryption_changed(sdc_ble_t *sb, int on)
{
  if(!sb->cs.fixed_key)
    return 0;
  // Encryption up: read remote CS caps (needs encryption), chaining
  // security -> config -> procedure.
  if(on && sb->cs.want_cs && sb->con.role == 0)
    cs_read_remote_caps(sb);
  return 1;
}


// --- CLI --------------------------------------------------------------------

static error_t
cmd_cs_caps(cli_t *cli, int argc, char **argv)
{
  sdc_hci_cmd_le_cs_read_local_supported_capabilities_v2_return_t c;
  int q = irq_forbid(IRQ_LEVEL_NET);
  uint8_t status = sdc_hci_cmd_le_cs_read_local_supported_capabilities_v2(&c);
  irq_permit(q);
  if(status) {
    cli_printf(cli, "CS read caps failed: 0x%x\n", status);
    return ERR_NOT_READY;
  }
  cli_printf(cli, "Channel Sounding capabilities:\n");
  cli_printf(cli, "  configs:%d  antennas:%d  paths:%d  max_procedures:%d\n",
             c.num_config_supported, c.num_antennas_supported,
             c.max_antenna_paths_supported,
             c.max_consecutive_procedures_supported);
  cli_printf(cli, "  roles:0x%x (%s%s)  modes:0x%x  rtt_cap:0x%x\n",
             c.roles_supported,
             c.roles_supported & 1 ? "initiator " : "",
             c.roles_supported & 2 ? "reflector" : "",
             c.modes_supported, c.rtt_capability);
  cli_printf(cli, "  cs_sync_phys:0x%x  subfeatures:0x%x  tx_snr:0x%x\n",
             c.cs_sync_phys_supported, c.subfeatures_supported,
             c.tx_snr_capability);
  cli_printf(cli, "  t_ip1:0x%x t_ip2:0x%x t_fcs:0x%x t_pm:0x%x t_sw:%d\n",
             c.t_ip1_times_supported, c.t_ip2_times_supported,
             c.t_fcs_times_supported, c.t_pm_times_supported,
             c.t_sw_time_supported);
  return 0;
}

CLI_CMD_DEF_EXT("cs_caps", cmd_cs_caps, NULL,
                "Read local Channel Sounding capabilities");

// Start Channel Sounding. Run on BOTH ends while connected: the reflector (ACL
// peripheral) just enables its CS role; the initiator (ACL central) enables
// security and the CS events drive config -> procedure. Uses the fixed test
// LTK so two mios devices can encrypt the link without pairing.
static error_t
cmd_cs_start(cli_t *cli, int argc, char **argv)
{
  sdc_ble_t *sb = &g_sdc;
  if(!sb->con.connected) {
    cli_printf(cli, "Not connected\n");
    return ERR_NOT_CONNECTED;
  }
  int q = irq_forbid(IRQ_LEVEL_NET);
  sdc_hci_cmd_le_cs_set_default_settings_t ds = {
    .conn_handle = sb->con.handle,
    .role_enable = sb->con.role == 0 ? 0x1 : 0x2, // initiator / reflector
    .cs_sync_antenna_selection = 1,
    .max_tx_power = 10,
  };
  sdc_hci_cmd_le_cs_set_default_settings_return_t dsr;
  uint8_t s = sdc_hci_cmd_le_cs_set_default_settings(&ds, &dsr);
  if(s) {
    irq_permit(q);
    cli_printf(cli, "cs set_default_settings: 0x%x\n", s);
    return ERR_NOT_READY;
  }

  sb->cs.fixed_key = 1; // both ends use the fixed test LTK
  cs_ntones = 0;        // fresh tone accumulator for this CS session

  // Free the radio for CS: our connectable advertising set keeps running after
  // we connect as a central; its adv events contend with the CS subevent slots.
  sdc_adv_enable(0);

  if(sb->con.role == 0) {
    sb->cs.active = 1;
    sb->cs.want_cs = 1;
    // Complete the LL feature exchange first; the 0x04 completion then chains
    // encrypt -> caps -> security -> config -> procedure.
    cs_read_features(sb);
    s = sb->cs.status;
    irq_permit(q);
    cli_printf(cli, s ? "cs read_features: 0x%x\n" : "CS initiator: starting\n", s);
    return s ? ERR_NOT_READY : 0;
  }
  irq_permit(q);
  cli_printf(cli, "CS reflector ready\n");
  return 0;
}

CLI_CMD_DEF_EXT("cs_start", cmd_cs_start, NULL,
                "Start Channel Sounding (run on both ends)");

// Standalone CS radio probe: LE CS Test runs a CS procedure with no peer
// connection. Args let it be tuned on-device:
//   cs_test [mode ip1 ip2 fcs pm sw mode0 txpwr tone_cfg subev_len subev_iv]
static error_t
cmd_cs_test(cli_t *cli, int argc, char **argv)
{
  sdc_ble_t *sb = &g_sdc;
  sdc_adv_enable(0); // free the radio: no advertising during the test

  int a[11] = { 2, 80, 80, 120, 20, 0, 3, 0, 0, 5000, 0x10 };
  for(int i = 0; i < 11 && i + 1 < argc; i++)
    a[i] = atoi(argv[i + 1]);

  int q = irq_forbid(IRQ_LEVEL_NET);
  const sdc_hci_cmd_le_cs_test_t t = {
    .main_mode_type = a[0],
    .sub_mode_type = 0xff,
    .main_mode_repetition = 0,
    .mode_0_steps = a[6],
    .role = 0,
    .rtt_type = 0,
    .cs_sync_phy = 2,
    .cs_sync_antenna_selection = 1,
    .subevent_len = a[9],
    .subevent_interval = a[10],
    .max_num_subevents = 1,
    .transmit_power_level = a[7],
    .t_ip1_time = a[1],
    .t_ip2_time = a[2],
    .t_fcs_time = a[3],
    .t_pm_time = a[4],
    .t_sw_time = a[5],
    .tone_antenna_config_selection = a[8],
    .cs_enhancements = 0,
    .snr_control_initiator = 0xff,
    .snr_control_reflector = 0xff,
    .drbg_nonce = 0,
    .channel_map_repetition = 1,
    .override_config = 0,
    .override_params_length = 0,
  };
  uint8_t s = sdc_hci_cmd_le_cs_test(&t);
  sb->cs.step = "cs_test";
  sb->cs.status = s;
  irq_permit(q);
  cli_printf(cli, "cs_test m=%d ip1=%d ip2=%d fcs=%d pm=%d sw=%d: 0x%x\n",
             a[0], a[1], a[2], a[3], a[4], a[5], s);
  return s ? ERR_NOT_READY : 0;
}

CLI_CMD_DEF_EXT("cs_test", cmd_cs_test, NULL,
                "Run standalone LE CS Test (no peer) to probe the CS radio");

// Set this board's CS distance offset (cm), compensating the fixed on-board
// RF/PCB path delay that otherwise inflates every distance estimate. The SDC
// applies it as a frequency-linear phase rotation to this board's PCT tones, so
// the measured distance drops by the offset. Set on each end before cs_start;
// the two boards' offsets add along the round trip.
static error_t
cmd_cs_offset(cli_t *cli, int argc, char **argv)
{
  int cm = argc > 1 ? atoi(argv[1]) : 0;
  sdc_hci_cmd_vs_cs_params_set_t p = {
    .cs_param_type = SDC_HCI_VS_CS_PARAM_TYPE_CS_BOARD_DISTANCE_OFFSET_SET,
  };
  p.cs_param_data.cs_board_distance_offset_params.cs_board_distance_offset_cm = cm;
  int q = irq_forbid(IRQ_LEVEL_NET);
  uint8_t s = sdc_hci_cmd_vs_cs_params_set(&p);
  irq_permit(q);
  cli_printf(cli, "cs board distance offset = %d cm: 0x%x\n", cm, s);
  return s ? ERR_NOT_READY : 0;
}

CLI_CMD_DEF_EXT("cs_offset", cmd_cs_offset, NULL,
                "Set this board's CS distance offset in cm (calibration)");

// Zero the per-channel tone accumulator without tearing down the CS session, so
// a fresh average can be gathered (e.g. after moving the boards).
static error_t
cmd_cs_reset(cli_t *cli, int argc, char **argv)
{
  int q = irq_forbid(IRQ_LEVEL_NET);
  cs_ntones = 0;
  irq_permit(q);
  cli_printf(cli, "cs tone accumulator cleared\n");
  return 0;
}

CLI_CMD_DEF_EXT("cs_reset", cmd_cs_reset, NULL,
                "Clear the CS tone accumulator (start a fresh average)");

// Set the range calibration offset (cm), subtracted from the reported live
// range to remove the fixed board/antenna path delay.
static error_t
cmd_cs_cal(cli_t *cli, int argc, char **argv)
{
  if(argc > 1)
    cs_cal_cm = atoi(argv[1]);
  cli_printf(cli, "cs range calibration offset: %d cm\n", cs_cal_cm);
  return 0;
}

CLI_CMD_DEF_EXT("cs_cal", cmd_cs_cal, NULL,
                "Set range calibration offset in cm (subtracted from range)");

// Live range readout on the initiator: print a fresh estimate each time new
// peer tones arrive, until any key is pressed. Runs in the shell thread (float
// math is FPU-safe there).
static error_t
cmd_cs_range(cli_t *cli, int argc, char **argv)
{
  int count = argc > 1 ? atoi(argv[1]) : 0; // N samples, or 0 = until keypress
  if(count == 0)
    cli_printf(cli, "Live CS range (press any key to stop)\n");
  uint32_t last = 0;
  while(1) {
    if(count == 0 && cli_getc(cli, 0) >= 0) // any key -> stop (non-blocking)
      break;
    int fl = irq_forbid(IRQ_LEVEL_NET);
    uint32_t g = cs_snap_gen;
    irq_permit(fl);
    if(g == last) {
      usleep(20000);
      continue;
    }
    last = g;
    // Note: the ranging LED is driven only on the reflector (cs_send_tones), so
    // the two boards stay visually distinguishable.
    if(cs_range_once(cli) >= 0 && count > 0 && --count == 0)
      break;
  }
  return 0;
}

CLI_CMD_DEF_EXT("cs_range", cmd_cs_range, NULL,
                "Live distance from CS tones: cs_range [N] (N samples, else key)");

// Enable/disable headless CS reflector mode, persisted in the filesystem. In
// this mode the board auto-arms as a CS reflector (fixed test LTK) on every
// connection, so it can run on a battery with no console. Configure it once on
// USB, then move to battery.
static error_t
cmd_cs_reflector(cli_t *cli, int argc, char **argv)
{
  int on = argc > 1 ? (!strcmp(argv[1], "on") || atoi(argv[1])) : 1;
  cs_auto_reflect = on;
  if(on) {
    fs_file_t *f;
    if(!fs_open("/cs_reflector", FS_WRONLY | FS_CREAT | FS_TRUNC, &f)) {
      uint8_t b = 1;
      fs_write(f, &b, 1);
      fs_close(f);
    }
  } else {
    fs_remove("/cs_reflector");
  }
  cli_printf(cli, "headless CS reflector mode: %s (persisted)\n",
             on ? "on" : "off");
  return 0;
}

CLI_CMD_DEF_EXT("cs_reflector", cmd_cs_reflector, NULL,
                "Headless CS reflector mode on/off (battery, auto-arm)");

// After the filesystem is mounted (constructor > 5100): load the persisted
// headless-reflector mode.
static void __attribute__((constructor(5300)))
cs_late_init(void)
{
  fs_file_t *f;
  if(!fs_open("/cs_reflector", FS_RDONLY, &f)) {
    uint8_t b = 0;
    fs_read(f, &b, 1);
    fs_close(f);
    cs_auto_reflect = b;
  }
}

// Trigger the LL feature exchange (a CS-capability-exchange prerequisite).
static error_t
cmd_ble_feat(cli_t *cli, int argc, char **argv)
{
  sdc_ble_t *sb = &g_sdc;
  if(!sb->con.connected) {
    cli_printf(cli, "Not connected\n");
    return ERR_NOT_CONNECTED;
  }
  sdc_hci_cmd_le_read_remote_features_t r = { .conn_handle = sb->con.handle };
  int q = irq_forbid(IRQ_LEVEL_NET);
  uint8_t s = sdc_hci_cmd_le_read_remote_features(&r);
  irq_permit(q);
  cli_printf(cli, "read_remote_features: 0x%x\n", s);
  return s ? ERR_NOT_READY : 0;
}

CLI_CMD_DEF_EXT("ble_feat", cmd_ble_feat, NULL, "Trigger LL feature exchange");

// Dump the last CS subevent's captured per-channel tone I/Q (mode-2 PBR), one
// "channel i q quality" line per tone. Distance = phase slope of the combined
// (this device x peer) tone phase vs frequency; computed off-device from the
// initiator's and reflector's dumps for now (see support/cs_distance.py).
static error_t
cmd_cs_data(cli_t *cli, int argc, char **argv)
{
  static struct cs_tone snap[CS_MAX_TONES];
  int ql = irq_forbid(IRQ_LEVEL_NET);
  uint8_t n = cs_ntones;
  memcpy(snap, cs_tones, n * sizeof(snap[0]));
  irq_permit(ql);

  // "channel i q quality": i/q are the running average over `count` subevents;
  // quality is always 0 here (only good-quality tones are accumulated) so the
  // host script (support/cs_distance.py) keeps them.
  cli_printf(cli, "cs_tones %d\n", n);
  for(uint8_t k = 0; k < n; k++) {
    int c = snap[k].count ? snap[k].count : 1;
    cli_printf(cli, "%d %d %d 0\n",
               snap[k].channel, snap[k].isum / c, snap[k].qsum / c);
  }
  return 0;
}

CLI_CMD_DEF_EXT("cs_data", cmd_cs_data, NULL,
                "Dump last CS subevent tone I/Q (channel i q quality)");
