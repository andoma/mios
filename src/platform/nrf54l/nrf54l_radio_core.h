#pragma once

#include <stdint.h>

// nRF54L 2.4 GHz RADIO, the modern Nordic radio IP (register map differs a lot
// from the nRF52 one: events at 0x200+, config block at 0xE00+). Used by the
// native BLE link layer (nrf54l_radio.c); with ENABLE_NRF_SDC the SoftDevice
// Controller owns the radio instead.

#define RADIO_BASE 0x5008a000

#define RADIO_TASKS_TXEN       (RADIO_BASE + 0x000)
#define RADIO_TASKS_RXEN       (RADIO_BASE + 0x004)
#define RADIO_TASKS_START      (RADIO_BASE + 0x008)
#define RADIO_TASKS_DISABLE    (RADIO_BASE + 0x010)
#define RADIO_SUBSCRIBE_TXEN   (RADIO_BASE + 0x100)
#define RADIO_EVENTS_ADDRESS   (RADIO_BASE + 0x20c)
#define RADIO_EVENTS_END       (RADIO_BASE + 0x218)
#define RADIO_EVENTS_DISABLED  (RADIO_BASE + 0x220)
#define RADIO_EVENTS_CRCOK     (RADIO_BASE + 0x22c)
#define RADIO_PUBLISH_ADDRESS  (RADIO_BASE + 0x30c)
#define RADIO_PUBLISH_END      (RADIO_BASE + 0x318)
#define RADIO_SHORTS           (RADIO_BASE + 0x400)
#define RADIO_INTENSET00       (RADIO_BASE + 0x488)
#define RADIO_INTENCLR00       (RADIO_BASE + 0x48c)
#define RADIO_MODE             (RADIO_BASE + 0x500)
#define RADIO_DATAWHITE        (RADIO_BASE + 0x540)
#define RADIO_FREQUENCY        (RADIO_BASE + 0x708)
#define RADIO_TXPOWER          (RADIO_BASE + 0x710)
#define RADIO_RSSISAMPLE       (RADIO_BASE + 0x718)
#define RADIO_CRCSTATUS        (RADIO_BASE + 0xe0c)
#define RADIO_PCNF0            (RADIO_BASE + 0xe20)
#define RADIO_PCNF1            (RADIO_BASE + 0xe28)
#define RADIO_BASE0            (RADIO_BASE + 0xe2c)
#define RADIO_PREFIX0          (RADIO_BASE + 0xe34)
#define RADIO_TXADDRESS        (RADIO_BASE + 0xe3c)
#define RADIO_RXADDRESSES      (RADIO_BASE + 0xe40)
#define RADIO_CRCCNF           (RADIO_BASE + 0xe44)
#define RADIO_CRCPOLY          (RADIO_BASE + 0xe48)
#define RADIO_CRCINIT          (RADIO_BASE + 0xe4c)
#define RADIO_PACKETPTR        (RADIO_BASE + 0xed0)

#define RADIO_IRQ              138 // RADIO_0 NVIC line

// SHORTS bits (subset used by the link layer).
#define RADIO_SHORT_READY_START     (1 << 0)
#define RADIO_SHORT_PHYEND_DISABLE  (1 << 19)

#define RADIO_INT_END               (1 << 6) // INTENSET00.END

// Start the HFXO (the radio needs the crystal for an accurate carrier). Safe to
// call more than once.
void nrf54l_hfxo_start(void);

// Apply the BLE PHY preset to the radio. Per-channel state (FREQUENCY,
// whitening IV, access address) is set separately by the link layer.
void nrf54l_radio_use_ble(void);
