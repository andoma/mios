#pragma once

// Bring up the 2.4 GHz radio as a BLE advertiser (ADV_IND on the three
// advertising channels). Minimal: TX-only, no scan/connect. Enough to show up
// as a discoverable BLE device. name is the Complete Local Name advertised.
void nrf54l_radio_ble_adv_init(const char *name);
