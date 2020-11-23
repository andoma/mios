#pragma once

#include <mios/io.h>

spi_t *stm32f4_spi_create(int instance, gpio_t clk, gpio_t miso,
                          gpio_pull_t mosi);

