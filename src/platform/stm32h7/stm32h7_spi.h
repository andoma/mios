#pragma once

#include <mios/io.h>

spi_t *stm32h7_spi_create(unsigned int instance, gpio_t clk, gpio_t miso,
                          gpio_t mosi, gpio_output_speed_t speed);
