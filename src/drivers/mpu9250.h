#pragma once

#include <io.h>

typedef struct mpu9250 mpu9250_t;

typedef struct mpu9250_values {
  float gx, gy, gz; // Gyro
  float ax, ay, az; // Accelerometer
  float mx, my, mz; // Magnetometer
} mpu9250_values_t;

mpu9250_t *mpu9250_create(spi_t *bus, gpio_t nss, gpio_t irq);

error_t mpu9250_reset(mpu9250_t *dev);

error_t mpu9250_calibrate(mpu9250_t *dev);

error_t mpu9250_read(mpu9250_t *dev, mpu9250_values_t *values);

