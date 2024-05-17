#pragma once

#define NRF52_BLE_STATUS_CONNECTED 0x1

void nrf52_radio_ble_init(const char *name, void (*cb)(int flags));

void nrf52_radio_got_xtal(void);
