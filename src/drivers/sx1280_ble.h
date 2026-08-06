#pragma once

#include "sx1280.h"

// Non-connectable BLE advertising (beacons)

void sx1280_ble_adv_start(sx1280_t *s, const char *name, int sweep);

void sx1280_ble_adv_stop(sx1280_t *s);

void sx1280_ble_adv_stats(sx1280_t *s, uint32_t *tx_done,
                          uint32_t *tx_timeout, uint32_t *cmd_errors);
