#pragma once


#define I2C_BASE(x)   (0x40005400 + ((x) * 0x400))

struct i2c {
  uint32_t base_addr;
};


void i2c_init(struct i2c *i2c, uint32_t reg_base);

