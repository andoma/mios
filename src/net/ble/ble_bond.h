#pragma once

#include <stdint.h>

// Persistent BLE bonds (pairing keys that survive reconnects and reboots).
// The lookup path runs in interrupt context and only ever touches the RAM
// cache; persistence (littlefs) happens from thread context.

typedef struct ble_bond {
  uint8_t  valid;
  uint8_t  peer_addr_type;   // identity address type: 0 public, 1 random
  uint8_t  peer_addr[6];     // identity address (LSB first)
  uint8_t  ltk[16];          // LTK we distributed; the central reconnects with it
  uint8_t  rand[8];          // Rand identifying the LTK
  uint16_t ediv;             // EDIV identifying the LTK
  uint8_t  irk[16];          // peer IRK (resolve its private address); 0 = none
} ble_bond_t;

// Reconnect lookup: find the bond whose LTK the central selected via EDIV/Rand.
// RAM-only, safe from interrupt context. Returns 1 and fills *out on hit.
int ble_bond_find_by_ediv(uint16_t ediv, const uint8_t rand[8], ble_bond_t *out);

// Store a bond (replacing any bond for the same identity address) and persist.
// Thread context only. Returns 0 on success.
int ble_bond_add(const ble_bond_t *b);

// Number of stored bonds (for the CLI).
int ble_bond_count(void);

// Forget all bonds (thread context).
void ble_bond_clear(void);
