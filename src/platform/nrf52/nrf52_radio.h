#pragma once

#define NRF52_BLE_STATUS_CONNECTED 0x1

void nrf52_radio_ble_init(const char *name, void (*cb)(int flags));

#define NRF52_RADIO_PWR_HIGH 0
#define NRF52_RADIO_PWR_LOW  1
#define NRF52_RADIO_PWR_OFF  2

void nrf52_radio_power_mode(int mode);
