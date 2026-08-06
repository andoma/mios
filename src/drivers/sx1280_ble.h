#pragma once

#include "sx1280.h"

// Non-connectable BLE advertising (beacons)

void sx1280_ble_adv_start(sx1280_t *s, const char *name, int sweep);

void sx1280_ble_adv_stop(sx1280_t *s);

struct stream;

// Print counters (tx, scan_req, conn_ind, ...) to a stream
void sx1280_ble_adv_report(sx1280_t *s, struct stream *st);
