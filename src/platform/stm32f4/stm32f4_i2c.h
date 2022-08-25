#pragma once

#include <mios/io.h>

i2c_t *stm32f4_i2c_create(int instance, gpio_t scl, gpio_t sda,
                          gpio_pull_t pull, gpio_output_speed_t drive_strength);
