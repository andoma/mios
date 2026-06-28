#pragma once

// CPU clock is configured to 128 MHz by nrf54l_soc_init() (OSCILLATORS PLL).
#define CPU_SYSCLK_MHZ  128

#define CPU_SYSTICK_RVR (CPU_SYSCLK_MHZ * 1000000)

// irq_alias.h only defines irq_0..irq_239, so this must be <= 240.
// Covers SERIAL00 (74), SERIAL20-22 (198-200), GRTC (226-229), GPIOTE20 (218).
#define CORTEXM_IRQ_COUNT 240

#define PBUF_DATA_SIZE 64

#define PBUF_DEFAULT_COUNT 64
