#pragma once

#include <mios/io.h>
#include <mios/imu.h>

typedef struct a3g4250d a3g4250d_t;

// SA0 pin selects the LSb of the I2C address: 0x68 (SA0=0) or 0x69 (SA0=1).
a3g4250d_t *a3g4250d_create(i2c_t *bus, uint8_t address);

error_t a3g4250d_reset(a3g4250d_t *dev);

error_t a3g4250d_read(a3g4250d_t *dev, imu_values_t *values);
