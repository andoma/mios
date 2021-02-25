#pragma once

#include <mios/io.h>

typedef struct hdc1080 hdc1080_t;

hdc1080_t *hdc1080_create(i2c_t *bus);

error_t hdc1080_read(hdc1080_t *hdc, int *deci_degrees, int *rh_promille);
