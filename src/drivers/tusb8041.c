
#include "tusb8041.h"
#include <mios/io.h>
#include <stdlib.h>


#define POL_REG   0x0b
#define CONF_REG  0x05
#define PWR_REG   0x06
#define CONF_DONE 0xf8

struct tusb8041 {

  i2c_t *i2c;
  uint8_t address;

  uint8_t power;

};


error_t tusb8041_init(tusb8041_t *tusb, int swapped)
{
  if (swapped) {
   error_t err = i2c_write_u8(tusb->i2c, tusb->address,
			      POL_REG, 0x80 | swapped);
   if (err)
     return err;
  }

  error_t err = i2c_write_u8(tusb->i2c, tusb->address, CONF_REG, 0x00);
  if (err)
    return err;

  return i2c_write_u8(tusb->i2c, tusb->address, CONF_DONE, 0x01);

}


error_t usb_power(tusb8041_t *tusb, int on)
{
  const uint8_t reg = on ? 0x0f : 0x00;
  if(reg == tusb->power)
    return 0;

  error_t err = i2c_write_u8(tusb->i2c, tusb->address, PWR_REG, reg);
  if (err)
    return err;
  tusb->power = reg;
  return 0;
}

tusb8041_t *
tusb8041_create(i2c_t *i2c, uint8_t address)
{
  tusb8041_t *tusb = calloc(1, sizeof(tusb8041_t));

  tusb->i2c = i2c;
  tusb->address = address;

  return tusb;
}
