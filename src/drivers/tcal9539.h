#pragma once

#include <stdint.h>

struct xgpio_irq_mux;
struct xgpio;
struct i2c;

struct xgpio *tcal9539_create(struct i2c *i2c, uint8_t address,
                              struct xgpio_irq_mux *mux);
