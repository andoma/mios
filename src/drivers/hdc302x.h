#pragma once

#include <mios/error.h>
#include <stdint.h>

struct i2c;
struct climate_zone;

typedef struct hdc302x hdc302x_t;

hdc302x_t *hdc302x_create(struct i2c *i2c, uint8_t addr);

error_t hdc302x_read(hdc302x_t *hdc, struct climate_zone *cz);
