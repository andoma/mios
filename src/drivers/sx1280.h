#pragma once

#include <mios/io.h>

typedef struct sx1280 sx1280_t;

sx1280_t *sx1280_create(spi_t *bus, gpio_t nss, gpio_t nreset,
                        gpio_t busy, gpio_t dio1, const char *name);

error_t sx1280_reset(sx1280_t *s);

error_t sx1280_read_reg(sx1280_t *s, uint16_t addr, void *ptr, size_t len);

error_t sx1280_write_reg(sx1280_t *s, uint16_t addr, const void *ptr,
                         size_t len);
