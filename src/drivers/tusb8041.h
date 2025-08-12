#pragma once

#include <mios/io.h>

typedef struct tusb8041 tusb8041_t;

#define TUSB8041_SWAPPED_US    0x01
#define TUSB8041_SWAPPED_DS_1  0x02
#define TUSB8041_SWAPPED_DS_2  0x04
#define TUSB8041_SWAPPED_DS_3  0x08
#define TUSB8041_SWAPPED_DS_4  0x10

#define TUSB8041_SWAPPED_DS    0x1e


error_t tusb8041_init(tusb8041_t *tusb, int flags);

tusb8041_t* tusb8041_create(i2c_t *i2c, uint8_t address);
