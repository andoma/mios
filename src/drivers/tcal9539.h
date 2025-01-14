#pragma once

#include <stdint.h>

struct indirect_gpio;
struct i2c;

struct indirect_gpio *tcal9539_create(struct i2c *i2c, uint8_t address);
