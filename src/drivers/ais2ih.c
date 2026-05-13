#include "ais2ih.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#define AIS2IH_REG_WHO_AM_I 0x0f
#define AIS2IH_REG_CTRL1    0x20
#define AIS2IH_REG_CTRL2    0x21
#define AIS2IH_REG_CTRL6    0x25
#define AIS2IH_REG_OUT_X_L  0x28

#define AIS2IH_WHO_AM_I_VALUE 0x44

// CTRL1: ODR=0101 (100 Hz), MODE=01 (high-performance, 14-bit), LP_MODE=00.
#define AIS2IH_CTRL1_HP_100HZ 0x54

// CTRL2: IF_ADD_INC=1 (default), BDU=0, all other bits at default.
#define AIS2IH_CTRL2_DEFAULT 0x04

// CTRL6: BW_FILT=00 (ODR/2), FS=00 (±2 g), FDS=0, LOW_NOISE=0.
#define AIS2IH_CTRL6_FS_2G 0x00

// Output registers are 14-bit data left-justified into a 16-bit two's
// complement word, so 1 LSB of the int16 corresponds to 0.244/4 = 0.061 mg
// at ±2 g full scale in high-performance mode.
#define AIS2IH_G_PER_LSB_2G (0.244e-3f / 4.0f)

struct ais2ih {
  i2c_t *i2c;
  uint8_t addr;
};


ais2ih_t *
ais2ih_create(i2c_t *bus, uint8_t address)
{
  ais2ih_t *dev = malloc(sizeof(ais2ih_t));
  if(dev == NULL)
    return NULL;
  dev->i2c = bus;
  dev->addr = address;
  return dev;
}


error_t
ais2ih_reset(ais2ih_t *dev)
{
  uint8_t id = 0;
  error_t err = i2c_read_u8(dev->i2c, dev->addr, AIS2IH_REG_WHO_AM_I, &id);
  if(err)
    return err;
  if(id != AIS2IH_WHO_AM_I_VALUE)
    return ERR_NO_DEVICE;

  err = i2c_write_u8(dev->i2c, dev->addr, AIS2IH_REG_CTRL2,
                     AIS2IH_CTRL2_DEFAULT);
  if(err)
    return err;

  err = i2c_write_u8(dev->i2c, dev->addr, AIS2IH_REG_CTRL6,
                     AIS2IH_CTRL6_FS_2G);
  if(err)
    return err;

  return i2c_write_u8(dev->i2c, dev->addr, AIS2IH_REG_CTRL1,
                      AIS2IH_CTRL1_HP_100HZ);
}


error_t
ais2ih_read(ais2ih_t *dev, imu_values_t *v)
{
  uint8_t buf[6];
  error_t err = i2c_read_bytes(dev->i2c, dev->addr, AIS2IH_REG_OUT_X_L,
                               buf, sizeof(buf));
  if(err)
    return err;

  const int16_t ix = (int16_t)(buf[0] | (buf[1] << 8));
  const int16_t iy = (int16_t)(buf[2] | (buf[3] << 8));
  const int16_t iz = (int16_t)(buf[4] | (buf[5] << 8));

  v->ax = ix * AIS2IH_G_PER_LSB_2G;
  v->ay = iy * AIS2IH_G_PER_LSB_2G;
  v->az = iz * AIS2IH_G_PER_LSB_2G;

  return 0;
}
