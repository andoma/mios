#pragma once

#include <mios/io.h>

// SPIM master driver for the nRF54L. base_addr/irq identify the instance
// (e.g. SPIM00 = 0x5004a000, IRQ 74), base_clock is the peripheral source
// clock in Hz (SPIM00 = 128 MHz) used to derive the SCK prescaler. Chip
// select is handled per-transfer as a plain GPIO (the nss argument), so
// PSEL.CSN is left disconnected.
spi_t *nrf54l_spi_create(uint32_t base_addr, int irq, uint32_t base_clock,
                         gpio_t clk, gpio_t miso, gpio_t mosi);
