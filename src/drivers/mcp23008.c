#include "mcp23008.h"

#include <stdlib.h>
#include <unistd.h>

#include <mios/device.h>

struct mcp23008 {
  device_t dev;
  i2c_t *i2c;
  uint8_t addr;
  uint8_t pullup;
  uint8_t configured;
  gpio_t reset;
};


const device_class_t mcp23008_dev_class = {

};



error_t
mcp23008_read(mcp23008_t *d, uint8_t *output)
{
  if(!d->configured) {
    d->configured = 1;
    error_t err = i2c_write_u8(d->i2c, d->addr, 0x6, 0xff);
    if(err)
      return err;
  }

  return i2c_read_u8(d->i2c, d->addr, 0x9, output);
}

mcp23008_t *
mcp23008_create(i2c_t *i2c, uint8_t addr, gpio_t reset)
{
  mcp23008_t *d = calloc(1, sizeof(mcp23008_t));

  d->dev.d_name = "mcp23008";
  d->dev.d_class = &mcp23008_dev_class;
  device_register(&d->dev);
  d->i2c = i2c;
  d->addr = addr;
  d->reset = reset;
  gpio_conf_output(d->reset, GPIO_OPEN_DRAIN,
                   GPIO_SPEED_LOW, GPIO_PULL_UP);
  gpio_set_output(d->reset, 0);
  udelay(100);
  gpio_set_output(d->reset, 1);

  return d;
}
