#pragma once

#include <mios/io.h>

#define SPI_CR1    0x00
#define SPI_CR2    0x04
#define SPI_CFG1   0x08
#define SPI_CFG2   0x0c
#define SPI_IER    0x10
#define SPI_SR     0x14
#define SPI_IFCR   0x18
#define SPI_TXDR   0x20
#define SPI_RXDR   0x30

#define SPI1_BASE 0x40013000
#define SPI2_BASE 0x40003800
#define SPI3_BASE 0x40003c00

spi_t *stm32h7_spi_create(unsigned int instance, gpio_t clk, gpio_t miso,
                          gpio_t mosi, gpio_output_speed_t speed);
