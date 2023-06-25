#pragma once

#include <mios/io.h>

struct block_iface;

struct block_iface *spiflash_create(spi_t *spi, gpio_t cs);
