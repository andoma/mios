#pragma once

#include <mios/io.h>

struct tps92682;

struct tps92682 *tps92682_create(spi_t *spi, gpio_t cs);

error_t tps92682_init(struct tps92682 *drv);

error_t tps92682_set_ilimit_raw(struct tps92682 *drv, unsigned int channel,
                                unsigned int value);
