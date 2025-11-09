#include "ads101x.h"

#include <unistd.h>
#include <stdio.h>

#include <mios/io.h>

// https://www.ti.com/lit/ds/symlink/ads1015-q1.pdf

#define REG_CONVERSION 0
#define REG_CONFIG     1
#define REG_LO_THRES   2
#define REG_HI_THRES   3

int
ads101x_sample(i2c_t *i2c, uint8_t i2c_addr, uint16_t config)
{
  config |= (0b100 << 5); // Conversion speed: 1600 SPS
  config |= (1 << 8);     // Single shot
  config |= (1 << 15);    // Start conversion

  uint8_t buf[3];
  buf[0] = REG_CONFIG;
  buf[1] = config >> 8;
  buf[2] = config;

  error_t err = i2c_rw(i2c, i2c_addr, buf, sizeof(buf), NULL, 0);
  if(err)
    return err;
  usleep(625); // Sleep for 1/1600 seconds just to avoid dumb-polling

  // Check that conversion has completed
  while(1) {
    err = i2c_rw(i2c, i2c_addr, buf, 1, buf + 1, 2);
    if(err)
      return err;

    if(buf[1] & 0x80)
      break;
  }

  buf[0] = REG_CONVERSION;
  err = i2c_rw(i2c, i2c_addr, buf, 1, buf + 1, 2);
  if(err)
    return err;
  return ((buf[1] << 8) | buf[2]) >> 4;
}


static const float lsb[8] = {
  0.003f, 0.002f, 0.001f, 0.0005f,
  0.00025f, 0.000125f, 0.000125f, 0.000125f
};

error_t
ads101x_sample_flt(i2c_t *i2c, uint8_t i2c_addr, uint16_t config, float *output)
{
  int val = ads101x_sample(i2c, i2c_addr, config);
  if(val < 0)
    return val;

  float scale = lsb[(config >> 9) & 7];
  *output = scale * val;
  return 0;
}
