#pragma once

#include <mios/io.h>

struct tps92682;

struct tps92682 *tps92682_create(spi_t *spi, gpio_t cs);

error_t tps92682_init(struct tps92682 *drv);

error_t tps92682_set_ilimit_raw(struct tps92682 *drv, unsigned int channel,
                                unsigned int value);

error_t tps92682_write_reg(struct tps92682 *drv, uint8_t reg, uint8_t value);

int tps92682_read_reg(struct tps92682 *drv, uint8_t reg);

error_t tps92682_set_spread_specturm(struct tps92682 *t, unsigned int magnitude);
