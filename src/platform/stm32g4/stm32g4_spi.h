#pragma once

#include <mios/io.h>

#define SPI_CR1    0x00
#define SPI_CR2    0x04
#define SPI_SR     0x08
#define SPI_DR     0x0c

spi_t *
stm32g4_spi_create(unsigned int instance, gpio_t clk, gpio_t miso,
                   gpio_pull_t mosi);
