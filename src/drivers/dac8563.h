#pragma once

#include <stdbool.h>

#include <mios/io.h>
#include <mios/imu.h>

struct stream;

typedef struct dac8563 dac8563_t;

dac8563_t *dac8563_create(spi_t *bus, gpio_t nss);

error_t dac8563_power(dac8563_t *dac, uint8_t channel, bool on);

error_t dac8563_set_dac(dac8563_t *dac, uint8_t channel, uint16_t value);

