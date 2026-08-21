#pragma once

#include <mios/io.h>
#include <mios/imu.h>

struct stream;

typedef struct icm42688 icm42688_t;

// bus/CS setup only, does not touch the chip
icm42688_t *icm42688_create(spi_t *bus, gpio_t nss);

// Soft-resets the chip, verifies WHO_AM_I, and configures accel+gyro
// for Low Noise mode, +-16g / +-2000dps, 1kHz ODR. Blocks ~50ms while
// the sensor start-up completes.
error_t icm42688_reset(icm42688_t *dev);

error_t icm42688_read(icm42688_t *dev, imu_values_t *values);

void icm42688_dump(icm42688_t *dev, struct stream *st);
