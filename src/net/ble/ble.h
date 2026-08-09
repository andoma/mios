#pragma once

#include <stdint.h>

// Stack-independent BLE CLI surface. Every BLE stack (nrf_sdc, sx1280)
// implements ble_print_connections() so the ble_connections command
// looks the same everywhere: per connection, a header line via
// ble_print_connection_header(), any stack-specific detail lines, and
// the channels via l2cap_print(). Radio-level state and counters do
// NOT belong here; they go in the stack's device print (`dev`).

struct stream;

void ble_print_connections(struct stream *st);

// addr is 6 bytes, least significant byte first (HCI order).
// sec_level is BLE_SEC_* (mios/service.h); intervals in microseconds.
void ble_print_connection_header(struct stream *st, int idx,
                                 const uint8_t *addr, const char *state,
                                 int sec_level, int interval_us,
                                 int timeout_us);
