#include "ist8310.h"
#include <malloc.h>
#include <unistd.h>
#include <stdio.h>

#define REG_WAI     0x00
#define REG_STAT1   0x02
#define REG_DATAXL  0x03 // X/Y/Z data, 6 bytes burst
#define REG_CNTL1   0x0a
#define REG_CNTL2   0x0b
#define REG_AVGCNTL 0x41
#define REG_PDCNTL  0x42

#define WAI_VALUE   0x10

#define STAT1_DRDY  (1 << 0)

#define CNTL1_MODE_STANDBY 0x00
#define CNTL1_MODE_SINGLE  0x01

#define CNTL2_SRST (1 << 0)

// Datasheet 3.1.1: 16x averaging on X/Y/Z for lowest noise (6ms/166Hz
// minimum measurement interval, vs. 5ms/200Hz default/4x).
#define AVGCNTL_LOW_NOISE 0x24
// Datasheet 3.1.1: "Normal" pulse duration -- "please use this setting".
#define PDCNTL_NORMAL 0xc0

// Resolution, datasheet sec 4.3 (0.3 uT/LSB, i.e. ~3.3 LSB/uT)
#define IST8310_SCALE 0.3f

struct ist8310 {
  i2c_t *bus;
  uint8_t addr;
};


ist8310_t *
ist8310_create(i2c_t *bus, uint8_t i2c_addr)
{
  ist8310_t *dev = xalloc(sizeof(ist8310_t), 0, 0);
  dev->bus = bus;
  dev->addr = i2c_addr;
  return dev;
}


error_t
ist8310_reset(ist8310_t *dev)
{
  error_t err = i2c_write_u8(dev->bus, dev->addr, REG_CNTL2, CNTL2_SRST);
  if(err)
    return err;
  usleep(50000); // Datasheet: POR takes up to 50ms

  uint8_t wai;
  err = i2c_read_u8(dev->bus, dev->addr, REG_WAI, &wai);
  if(err)
    return err;
  if(wai != WAI_VALUE)
    return ERR_MISMATCH;

  err = i2c_write_u8(dev->bus, dev->addr, REG_AVGCNTL, AVGCNTL_LOW_NOISE);
  if(err)
    return err;
  return i2c_write_u8(dev->bus, dev->addr, REG_PDCNTL, PDCNTL_NORMAL);
}


error_t
ist8310_read(ist8310_t *dev, float *mx, float *my, float *mz)
{
  error_t err = i2c_write_u8(dev->bus, dev->addr, REG_CNTL1,
                             CNTL1_MODE_SINGLE);
  if(err)
    return err;

  // No DRDY line wired on fc1, so poll STAT1 instead. ~6ms typical with
  // the low-noise averaging config; give it some margin.
  uint8_t stat1 = 0;
  for(int i = 0; i < 15; i++) {
    usleep(1000);
    if(i2c_read_u8(dev->bus, dev->addr, REG_STAT1, &stat1))
      continue;
    if(stat1 & STAT1_DRDY)
      break;
  }
  if(!(stat1 & STAT1_DRDY))
    return ERR_TIMEOUT;

  uint8_t buf[6];
  err = i2c_read_bytes(dev->bus, dev->addr, REG_DATAXL, buf, sizeof(buf));
  if(err)
    return err;

  const int16_t ix = buf[0] | (buf[1] << 8);
  const int16_t iy = buf[2] | (buf[3] << 8);
  const int16_t iz = buf[4] | (buf[5] << 8);

  *mx = ix * IST8310_SCALE;
  *my = iy * IST8310_SCALE;
  *mz = iz * IST8310_SCALE;
  return 0;
}


void
ist8310_dump(ist8310_t *dev, stream_t *st)
{
  uint8_t wai;
  error_t err = i2c_read_u8(dev->bus, dev->addr, REG_WAI, &wai);
  if(err) {
    stprintf(st, "WHO_AM_I read failed: %d\n", err);
    return;
  }
  stprintf(st, "WHO_AM_I: 0x%02x\n", wai);

  float mx, my, mz;
  err = ist8310_read(dev, &mx, &my, &mz);
  if(err) {
    stprintf(st, "Read failed: %d\n", err);
    return;
  }
  stprintf(st, "Mag: %8.2f %8.2f %8.2f  uT\n", mx, my, mz);
}
