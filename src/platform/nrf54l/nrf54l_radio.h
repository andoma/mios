#pragma once

// Bring up the 2.4 GHz radio as a connectable BLE peripheral: advertises
// ADV_IND and, on CONNECT_IND, runs the link-layer connection (data channels,
// l2cap) like the nRF52 port. name is the Complete Local Name advertised.
void nrf54l_radio_ble_init(const char *name);

// RADIO interrupt handler for BLE; called by the arbiter while BLE owns radio.
void nrf54l_ble_radio_irq(void);
