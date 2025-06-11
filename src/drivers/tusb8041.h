#pragma once

#include <mios/io.h>

typedef struct tusb8041 tusb8041_t;

error_t tusb8041_init(tusb8041_t *tusb, int swapped);

tusb8041_t* tusb8041_create(i2c_t *i2c, uint8_t address);
