#pragma once

// Radio arbiter: time-division-multiplexes the single radio between a timed
// client (BLE, which acquires the radio for each event and releases it when
// idle) and a background client (802.15.4, which gets the radio whenever BLE
// is not using it). The arbiter owns the radio interrupt and dispatches it to
// whichever client currently holds the radio.

typedef struct radio_client {
  void (*suspend)(void); // give up the radio now (stop RX/TX, disable)
  void (*resume)(void);  // the radio is yours again (PHY already set); start
  void (*irq)(void);     // radio interrupt while this client is the owner
} radio_client_t;

void nrf54l_radio_arb_init(void);

// BLE registers its interrupt handler; it drives ownership via acquire/release.
void nrf54l_radio_arb_set_ble(const radio_client_t *c);

// The background (802.15.4) client; it is granted the radio immediately if idle.
void nrf54l_radio_arb_set_background(const radio_client_t *c);

// BLE takes the radio for an event (suspends background, switches PHY to BLE).
void nrf54l_radio_arb_acquire(void);

// BLE returns the radio (switches PHY to 15.4, resumes background RX).
void nrf54l_radio_arb_release(void);
