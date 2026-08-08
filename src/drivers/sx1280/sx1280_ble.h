#pragma once

#include "sx1280.h"

// Non-connectable BLE advertising (beacons)

void sx1280_ble_adv_start(sx1280_t *s, const char *name, int sweep);

void sx1280_ble_adv_stop(sx1280_t *s);

struct stream;

// Print counters (tx, scan_req, conn_ind, ...) to a stream
void sx1280_ble_adv_report(sx1280_t *s, struct stream *st);

// T_IFS calibration: SetAutoTx arm value used for connections
void sx1280_ble_set_autotx(sx1280_t *s, int val);

// Cap the advertised DLE payload (27..126 octets); affects new
// connections' LENGTH exchanges only
void sx1280_ble_set_dle(sx1280_t *s, int octets);

void sx1280_ble_set_txpower(sx1280_t *s, int dbm);

int sx1280_ble_conn_active(sx1280_t *s);

// Terminate the current connection (LL_TERMINATE_IND)
void sx1280_ble_conn_drop(sx1280_t *s);
