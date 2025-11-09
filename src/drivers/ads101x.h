#pragma once

#include <mios/error.h>
#include <stdint.h>

struct i2c;

#define ADSD101X_MUX_AIN0_AIN1  (0b000 << 12)
#define ADSD101X_MUX_AIN0_AIN3  (0b001 << 12)
#define ADSD101X_MUX_AIN1_AIN3  (0b010 << 12)
#define ADSD101X_MUX_AIN2_AIN3  (0b011 << 12)
#define ADSD101X_MUX_AIN0_GND   (0b100 << 12)
#define ADSD101X_MUX_AIN1_GND   (0b101 << 12)
#define ADSD101X_MUX_AIN2_GND   (0b110 << 12)
#define ADSD101X_MUX_AIN3_GND   (0b111 << 12)

#define ADSD101X_PGA_6V144      (0b000 << 9)
#define ADSD101X_PGA_4V096      (0b001 << 9)
#define ADSD101X_PGA_2V048      (0b010 << 9)
#define ADSD101X_PGA_1V024      (0b011 << 9)
#define ADSD101X_PGA_0V512      (0b100 << 9)
#define ADSD101X_PGA_0V256      (0b101 << 9)

int ads101x_sample(struct i2c *i2c, uint8_t i2c_addr, uint16_t config);

error_t ads101x_sample_flt(struct i2c *i2c, uint8_t i2c_addr, uint16_t config,
                           float *output);
