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

#include <mios/cli.h>
#include <mios/error.h>

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
  // A LIMITED channel set (36 channels, 26..61) keeps the step count per
  // procedure small enough to schedule/report, matching the working Zephyr
  // connected_cs sample. bit i of byte i/8 = channel i.
  static const uint8_t cs_chmap[10] = {
    0x00, 0x00, 0x00, // ch 0-23
    0xfc,             // ch 24-31: channels 26-31
    0xff, 0xff, 0xff, // ch 32-55
    0x3f,             // ch 56-63: channels 56-61
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
    .min_procedure_interval = 100,  // connection events
    .max_procedure_interval = 100,
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
#define CS_MAX_TONES 72
struct cs_tone {
  uint8_t channel;
  uint8_t quality;
  int16_t i;
  int16_t q;
};
static struct cs_tone cs_tones[CS_MAX_TONES];
static uint8_t cs_ntones;

static void
cs_store_tone(uint8_t channel, const uint8_t *tone)
{
  if(cs_ntones >= CS_MAX_TONES)
    return;
  uint32_t pct = tone[0] | (tone[1] << 8) | (tone[2] << 16);
  int16_t i = (int16_t)((pct & 0x000fff) ^ 0x800) - 0x800;
  int16_t q = (int16_t)(((pct & 0xfff000) >> 12) ^ 0x800) - 0x800;
  cs_tones[cs_ntones].channel = channel;
  cs_tones[cs_ntones].quality = tone[3] & 0x0f; // quality_indicator (low nibble)
  cs_tones[cs_ntones].i = i;
  cs_tones[cs_ntones].q = q;
  cs_ntones++;
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
    // Start of a subevent report: reset the tone capture and parse its steps.
    // data[] length = (LE-meta params) - subevent code - fixed struct fields.
    cs_ntones = 0;
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

  cli_printf(cli, "cs_tones %d\n", n);
  for(uint8_t k = 0; k < n; k++)
    cli_printf(cli, "%d %d %d %d\n",
               snap[k].channel, snap[k].i, snap[k].q, snap[k].quality);
  return 0;
}

CLI_CMD_DEF_EXT("cs_data", cmd_cs_data, NULL,
                "Dump last CS subevent tone I/Q (channel i q quality)");
