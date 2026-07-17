#pragma once

// CPU clock is configured to 128 MHz by nrf54l_soc_init() (OSCILLATORS PLL).
#define CPU_SYSCLK_MHZ  128

#define CPU_SYSTICK_RVR (CPU_SYSCLK_MHZ * 1000000)

// Covers all nRF54L15 NVIC IRQs (highest is GPIOTE30_1 = 269), including the
// LP-domain peripherals at IDs >= 256 (UARTE30/SERIAL30 = 260, GPIOTE30, etc).
// irq_alias.h defines irq_0..irq_287, the cortexm NVIC masks handle >256.
#define CORTEXM_IRQ_COUNT 270

#define EVENTLOG_SIZE 4096

// Big enough to hold the largest single L2CAP PDU (the 65-byte LESC Pairing
// Public Key plus headers) in one pbuf. The controller still fragments to the
// over-air LL PDU size, so the peer need not support Data Length Extension.
#define PBUF_DATA_SIZE 128

#define PBUF_DEFAULT_COUNT 64

// Software interrupts (used by the BLE radio driver to defer adv re-entry)
#define NUM_SOFTIRQ 4
