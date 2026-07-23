#pragma once

// Shared state and hooks between the core SDC glue (nrf_sdc.c) and the optional
// Channel Sounding extension (nrf_sdc_cs.c, built only when ENABLE_BLE_CS). The
// CS hooks are weak no-ops in nrf_sdc.c; nrf_sdc_cs.c provides strong overrides
// when compiled in. This keeps CS out of the binary (support calls, glue, CLI,
// controller CS code via --gc-sections) on builds that do not enable it.

#include <stdint.h>

#include "net/netif.h"
#include "net/ble/l2cap.h"

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
    uint8_t role; // 0 = central, 1 = peripheral
  } con;

  uint8_t sb_tx_credits;  // ACL packets the controller can accept right now
  uint8_t sb_tx_ceiling;  // from LE Read Buffer Size

  uint8_t sb_advertising;
  uint8_t sb_scanning;
  uint8_t sb_connecting;
  uint8_t sb_addr[6];
  char sb_target[24]; // name prefix the central role scans/connects for

  // Channel Sounding state (driven as a state machine by the CS events).
  // Only touched by nrf_sdc_cs.c; a few unused bytes when CS is not built.
  struct {
    uint8_t active;
    uint8_t want_cs;    // start CS once the link is encrypted (initiator)
    uint8_t fixed_key;  // use the fixed test LTK for this link (CS test)
    const char *step;
    int status;
    uint16_t procedures;
    uint8_t last_steps;
  } cs;

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

  // Bring-up diagnostics: if a controller/HCI setup step fails we record it
  // here and keep the system booting (instead of panicking, which would also
  // take down the console/MCP) so it can be read back over "dev".
  const char *sb_err_step;
  int sb_err_status;

} sdc_ble_t;

extern sdc_ble_t g_sdc;

// Core helpers the CS extension calls into.
void sdc_ltk_reply(l2cap_t *l2c, const uint8_t *ltk);
uint8_t sdc_adv_enable(uint8_t on);

// Channel Sounding hooks. Weak no-op stubs live in nrf_sdc.c; nrf_sdc_cs.c
// overrides them when built. Called from the corresponding core paths.
int  nrf_sdc_cs_configure(void);                       // sdc_support_*/cfg (init)
void nrf_sdc_cs_setup_hci(void);                        // host feature + timing
void nrf_sdc_cs_connected(sdc_ble_t *sb);               // arm reflector on connect
int  nrf_sdc_cs_le_meta(sdc_ble_t *sb, const uint8_t *p); // CS LE subevents -> 1
int  nrf_sdc_cs_ltk(sdc_ble_t *sb);                     // fixed-key LTK reply -> 1
int  nrf_sdc_cs_encryption_changed(sdc_ble_t *sb, int on); // consume enc-change -> 1
