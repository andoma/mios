#pragma once

#include <mios/io.h>

#define SPI1_BASE 0x40013000
#define SPI3_BASE 0x40003c00

#define SPI_CR1    0x00
#define SPI_CR2    0x04
#define SPI_SR     0x08
#define SPI_DR     0x0c
#define SPI_CRCPR  0x10
#define SPI_RXCRC  0x14
#define SPI_TXCRC  0x18
#define SPI_I2S    0x1c
#define SPI_I2SPR  0x20



spi_t *stm32f4_spi_create(unsigned int instance, gpio_t clk, gpio_t miso,
                          gpio_t mosi, gpio_output_speed_t speed);

