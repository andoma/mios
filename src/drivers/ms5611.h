#pragma once

#include "i2c.h"

typedef struct ms5611 ms5611_t;

typedef struct ms5611_value {
  float temp;      // In celsius
  float pressure;  // In mBar
} ms5611_value_t;

error_t ms5611_create(i2c_t *bus, ms5611_t **dev);

error_t ms5611_read(ms5611_t *dev, ms5611_value_t *values);
