#pragma once

#include <stdint.h>
#include <stddef.h>

// Shared glue for Nordic's SoftDevice Controller (binary blob, sdk-nrfxlib).
// Everything from the Bluetooth HCI boundary up (HCI <-> l2cap pump) is
// SoC-independent and lives in nrf_sdc.c. Each SoC (nrf52, nrf54l) provides
// the small hardware layer below via the hooks declared here.

// Board entry point: bring up BLE as a connectable peripheral advertising the
// Complete Local Name `name`. Called from board init with interrupts masked;
// the controller is brought up later on the main thread.
void nrf_ble_init(const char *name);

// --- SoC hooks (implemented per SoC: nrf52_mpsl.c / nrf54l_mpsl.c etc.) -----

// Bring up Nordic's Multiprotocol Service Layer. low_prio is invoked from the
// MPSL low-priority interrupt at IRQ_LEVEL_NET; every SDC/MPSL "low priority"
// API call must run at that same execution priority.
void nrf_mpsl_init(void (*low_prio)(void));

// Pend the MPSL low-priority interrupt (runs low_prio soon, at NET level).
void nrf_mpsl_kick(void);

// TRNG used for the SDC rand source and SMP pairing nonces.
void nrf_trng_init(void);
void nrf_trng_read(uint8_t *buf, size_t len);

// Fill addr with this device's static random BLE address, derived from the
// factory FICR device address (MSByte's top two bits set to mark it static
// random). HCI byte order: LSB first.
void nrf_ficr_ble_addr(uint8_t addr[6]);
