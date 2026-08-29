#pragma once

#include <mios/io.h>

struct stream;

// 3-axis I2C magnetometer. On fc1 this sits on the same footprint (and
// I2C1 bus) originally laid out for a QMC5883P -- pin-for-pin compatible
// by design (SCL/SDA/VDD/GND all line up, and the footprint's lone
// capacitor-only pad is exactly the IST8310's required 4.7uF set/reset
// cap). CAD0/CAD1 are left floating, which the datasheet defines as
// I2C address 0x0e.
typedef struct ist8310 ist8310_t;

// bus/address setup only, does not touch the chip
ist8310_t *ist8310_create(i2c_t *bus, uint8_t i2c_addr);

// Soft-resets the chip, verifies the WHO_AM_I register and configures
// low-noise averaging (16x, per datasheet recommendation).
error_t ist8310_reset(ist8310_t *dev);

// Triggers a single-measurement conversion and polls for completion
// (chip has no interrupt line wired on fc1), so this blocks for roughly
// one conversion time (~6ms with the low-noise averaging config).
// Output is in microtesla.
error_t ist8310_read(ist8310_t *dev, float *mx, float *my, float *mz);

void ist8310_dump(ist8310_t *dev, struct stream *st);
