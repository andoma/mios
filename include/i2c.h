#pragma once

#include <stddef.h>
#include <stdint.h>

#include "error.h"

typedef struct i2c i2c_t;

error_t i2c_rw(i2c_t *i2c, uint8_t addr,
               const uint8_t *write, size_t write_len,
               uint8_t *read, size_t read_len);
