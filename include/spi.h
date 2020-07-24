#pragma once

#include <stddef.h>
#include <stdint.h>

#include "error.h"

typedef struct spi spi_t;

error_t spi_rw(spi_t *spi, const uint8_t *tx, uint8_t *rx, size_t len);
