#pragma once

#include <io.h>

typedef struct ms5611 ms5611_t;

typedef struct ms5611_value {
  float temp;      // In celsius
  float pressure;  // In mBar
} ms5611_value_t;

ms5611_t *ms5611_create(spi_t *bus, gpio_t nss);

error_t ms5611_init(ms5611_t *dev);

error_t ms5611_read(ms5611_t *dev, ms5611_value_t *values);
