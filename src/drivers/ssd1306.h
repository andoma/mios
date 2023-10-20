#pragma once

#include <mios/error.h>

typedef struct ssd1306 ssd1306_t;

struct i2c;

ssd1306_t *ssd1306_create(struct i2c *bus);

error_t ssd1306_init(ssd1306_t *dev);

error_t ssd1306_print(ssd1306_t *dev, int row, const char *str);
