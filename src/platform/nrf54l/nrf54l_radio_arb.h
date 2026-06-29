#pragma once

// Radio arbiter: time-division-multiplexes the single radio between BLE (the
// timed client, which acquires the radio for each event and releases it when
// idle between anchors) and 802.15.4 background RX (which receives whenever BLE
// is idle). It owns the radio interrupt and dispatches it to the current owner.
// Dispatch is direct to the two known clients, not through registration/
// function pointers: this is a two-protocol radio, not a plugin host.

void nrf54l_radio_arb_init(void);

// BLE takes the radio for an event (suspends 15.4, switches PHY to BLE).
void nrf54l_radio_arb_acquire(void);

// BLE returns the radio (switches PHY to 15.4, resumes background RX).
void nrf54l_radio_arb_release(void);

// Grant the radio to 802.15.4 background RX if nothing else holds it (called
// once at startup, after BLE init).
void nrf54l_radio_arb_background_start(void);
