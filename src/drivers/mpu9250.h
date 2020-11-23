#pragma once

#include <mios/io.h>
#include <mios/imu.h>

typedef struct mpu9250 mpu9250_t;

mpu9250_t *mpu9250_create(spi_t *bus, gpio_t nss, gpio_t irq);

error_t mpu9250_reset(mpu9250_t *dev);

error_t mpu9250_calibrate(mpu9250_t *dev);

error_t mpu9250_read(mpu9250_t *dev, imu_values_t *values);

