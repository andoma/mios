#pragma once

#include <mios/io.h>

spi_t *nrf52_spi_create(unsigned int spi_instance, gpio_t clk, gpio_t miso,
                        gpio_t mosi);
