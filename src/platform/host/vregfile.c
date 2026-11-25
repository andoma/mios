#include <stdint.h>
#include <stddef.h>

#include "vregfile.h"

// See vregfile.h

#define ST_IDLE     0
#define ST_POINTER  1   // I2C: the next written byte is the pointer
#define ST_WRITE    2
#define ST_READ     3

static vregfile_t *
from_i2c(vi2c_dev_t *d)
{
  return (vregfile_t *)((char *)d - offsetof(vregfile_t, dev.i2c));
}

static vregfile_t *
from_spi(vspi_dev_t *d)
{
  return (vregfile_t *)((char *)d - offsetof(vregfile_t, dev.spi));
}


// ---- I2C

static int
rf_i2c_start(vi2c_dev_t *d, uint8_t addr, int read)
{
  vregfile_t *rf = from_i2c(d);
  rf->state = read ? ST_READ : ST_POINTER;
  return 0;
}

static int
rf_i2c_write(vi2c_dev_t *d, uint8_t byte)
{
  vregfile_t *rf = from_i2c(d);
  if(rf->state == ST_POINTER) {
    rf->ptr = byte;
    rf->state = ST_WRITE;
  } else {
    rf->regs[rf->ptr++] = byte;
  }
  return 0;
}

static uint8_t
rf_i2c_read(vi2c_dev_t *d)
{
  vregfile_t *rf = from_i2c(d);
  return rf->regs[rf->ptr++];
}

static void
rf_i2c_stop(vi2c_dev_t *d)
{
  from_i2c(d)->state = ST_IDLE;
}

static const vi2c_dev_ops_t rf_i2c_ops = {
  .start = rf_i2c_start,
  .write = rf_i2c_write,
  .read = rf_i2c_read,
  .stop = rf_i2c_stop,
};


error_t
vregfile_attach_i2c(vregfile_t *rf, i2c_t *bus, uint8_t addr)
{
  rf->dev.i2c.ops = &rf_i2c_ops;
  rf->state = ST_IDLE;
  return vi2c_attach(bus, addr, &rf->dev.i2c);
}


// ---- SPI

static void
rf_spi_select(vspi_dev_t *d, int mode)
{
  from_spi(d)->state = ST_POINTER;
}

static uint8_t
rf_spi_xfer(vspi_dev_t *d, uint8_t mosi)
{
  vregfile_t *rf = from_spi(d);
  switch(rf->state) {
  case ST_POINTER:
    rf->ptr = mosi & 0x7f;
    rf->state = mosi & 0x80 ? ST_READ : ST_WRITE;
    return 0;
  case ST_READ:
    return rf->regs[rf->ptr++];
  case ST_WRITE:
    rf->regs[rf->ptr++] = mosi;
    return 0;
  default:
    return 0xff;
  }
}

static void
rf_spi_deselect(vspi_dev_t *d)
{
  from_spi(d)->state = ST_IDLE;
}

static const vspi_dev_ops_t rf_spi_ops = {
  .select = rf_spi_select,
  .xfer = rf_spi_xfer,
  .deselect = rf_spi_deselect,
};


error_t
vregfile_attach_spi(vregfile_t *rf, spi_t *bus, gpio_t cs)
{
  rf->dev.spi.ops = &rf_spi_ops;
  rf->state = ST_IDLE;
  return vspi_attach(bus, cs, &rf->dev.spi);
}
