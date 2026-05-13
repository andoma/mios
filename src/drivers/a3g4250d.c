#include "a3g4250d.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#define A3G4250D_REG_WHO_AM_I   0x0f
#define A3G4250D_REG_CTRL_REG1  0x20
#define A3G4250D_REG_CTRL_REG4  0x23
#define A3G4250D_REG_STATUS_REG 0x27
#define A3G4250D_REG_OUT_X_L    0x28

#define A3G4250D_WHO_AM_I_VALUE 0xd3

// Sub-address MSb enables register address auto-increment in multi-byte
// transfers.
#define A3G4250D_AUTO_INC       0x80

// Sensitivity from datasheet: 8.75 mdps/digit, converted to rad/s/digit.
//   8.75e-3 * (pi / 180) = 1.5271630954950384e-4
#define A3G4250D_RAD_PER_DIGIT 1.5271630954950384e-4f

struct a3g4250d {
  i2c_t *i2c;
  uint8_t addr;
};


a3g4250d_t *
a3g4250d_create(i2c_t *bus, uint8_t address)
{
  a3g4250d_t *dev = malloc(sizeof(a3g4250d_t));
  if(dev == NULL)
    return NULL;
  dev->i2c = bus;
  dev->addr = address;
  return dev;
}


error_t
a3g4250d_reset(a3g4250d_t *dev)
{
  uint8_t id = 0;
  error_t err = i2c_read_u8(dev->i2c, dev->addr, A3G4250D_REG_WHO_AM_I, &id);
  if(err)
    return err;
  if(id != A3G4250D_WHO_AM_I_VALUE)
    return ERR_NO_DEVICE;

  // CTRL_REG1: ODR=100Hz, BW=12.5, PD=1 (normal mode), X/Y/Z enabled.
  err = i2c_write_u8(dev->i2c, dev->addr, A3G4250D_REG_CTRL_REG1, 0x0f);
  if(err)
    return err;

  // CTRL_REG4: 4-wire SPI, no self-test, little-endian, default scale.
  return i2c_write_u8(dev->i2c, dev->addr, A3G4250D_REG_CTRL_REG4, 0x00);
}


error_t
a3g4250d_read(a3g4250d_t *dev, imu_values_t *v)
{
  uint8_t buf[6];
  error_t err = i2c_read_bytes(dev->i2c, dev->addr,
                               A3G4250D_REG_OUT_X_L | A3G4250D_AUTO_INC,
                               buf, sizeof(buf));
  if(err)
    return err;

  const int16_t ix = (int16_t)(buf[0] | (buf[1] << 8));
  const int16_t iy = (int16_t)(buf[2] | (buf[3] << 8));
  const int16_t iz = (int16_t)(buf[4] | (buf[5] << 8));

  v->wx = ix * A3G4250D_RAD_PER_DIGIT;
  v->wy = iy * A3G4250D_RAD_PER_DIGIT;
  v->wz = iz * A3G4250D_RAD_PER_DIGIT;

  return 0;
}
