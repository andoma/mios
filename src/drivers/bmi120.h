#pragma once

#include <mios/io.h>
#include <mios/imu.h>

struct stream;

typedef struct bmi120 bmi120_t;

bmi120_t *bmi120_create(spi_t *bus, gpio_t nss);

typedef enum {
  BMI120_ACC_RANGE_2G,
  BMI120_ACC_RANGE_4G,
  BMI120_ACC_RANGE_8G,
  BMI120_ACC_RANGE_16G,
} bmi120_acc_range_t;

error_t bmi120_reset(bmi120_t *dev, bmi120_acc_range_t acc_range);

error_t bmi120_read(bmi120_t *dev, imu_values_t *values);

void bmi120_dump(bmi120_t *dev, struct stream *st);
