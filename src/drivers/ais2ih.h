#pragma once

#include <mios/io.h>
#include <mios/imu.h>

typedef struct ais2ih ais2ih_t;

// SA0 pin selects the LSb of the I2C address: 0x18 (SA0=0) or 0x19 (SA0=1).
ais2ih_t *ais2ih_create(i2c_t *bus, uint8_t address);

error_t ais2ih_reset(ais2ih_t *dev);

error_t ais2ih_read(ais2ih_t *dev, imu_values_t *values);
