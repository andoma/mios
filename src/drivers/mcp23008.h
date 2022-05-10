#pragma once

#include <mios/io.h>

typedef struct mcp23008 mcp23008_t;

mcp23008_t *mcp23008_create(i2c_t *i2c, uint8_t addr, gpio_t reset);

error_t mcp23008_read(mcp23008_t *d, uint8_t *output);

error_t mcp23008_pullup(mcp23008_t *d, uint8_t mask);



