// TCAL9539-Q1 Automotive Low-Voltage 16-Bit I2C-Bus, SMBus I/O Expander

// https://www.ti.com/lit/ds/symlink/tcal9539-q1.pdf

#include "tcal9539.h"
#include <mios/io.h>
#include <stdlib.h>

#define TCAL9539_INPUT0  0x00
#define TCAL9539_INPUT1  0x01
#define TCAL9539_OUTPUT0 0x02
#define TCAL9539_OUTPUT1 0x03
#define TCAL9539_INV0    0x04
#define TCAL9539_INV1    0x05
#define TCAL9539_CFG0    0x06
#define TCAL9539_CFG1    0x07

#define TCAL9539_PE0     0x46
#define TCAL9539_PE1     0x47
#define TCAL9539_PUD0    0x48
#define TCAL9539_PUD1    0x49

typedef struct {
  indirect_gpio_t gpio;
  i2c_t *i2c;
  uint8_t address;
  uint16_t output;
  uint16_t direction;  // 1 = input (default)
  uint16_t pull_enable;
  uint16_t pull_direction;
} tcal9539_t;


static error_t
tcal9539_write_shadow(tcal9539_t *tc, unsigned int line,
                      uint16_t shadow, int reg)
{
  if(line < 8) {
    return i2c_write_u8(tc->i2c, tc->address, reg, shadow && 0xff);
  } else {
    return i2c_write_u8(tc->i2c, tc->address, reg + 1, shadow >> 8);
  }
}


static error_t
tcal9539_set_bit_in_reg(tcal9539_t *tc, unsigned int line,
                        uint16_t *shadow, int reg)
{
  uint16_t mask = (1 << line);
  if(*shadow & mask)
    return 0; // Already set

  *shadow |= mask;

  return tcal9539_write_shadow(tc, line, *shadow, reg);
}


static error_t
tcal9539_clr_bit_in_reg(tcal9539_t *tc, unsigned int line,
                        uint16_t *shadow, int reg)
{
  uint16_t mask = (1 << line);
  if(!(*shadow & mask))
    return 0; // Already cleared

  *shadow &= ~mask;
  return tcal9539_write_shadow(tc, line, *shadow, reg);
}


static error_t
tcal9539_conf_pull(tcal9539_t *tc, unsigned int line, gpio_pull_t pull)
{
  if(pull == GPIO_PULL_NONE) {
    return tcal9539_clr_bit_in_reg(tc, line, &tc->pull_enable, TCAL9539_PE0);
  } else {

    error_t err;
    if(pull == GPIO_PULL_UP) {
      err = tcal9539_set_bit_in_reg(tc, line, &tc->pull_direction,
                                    TCAL9539_PUD0);
    } else {
      err = tcal9539_clr_bit_in_reg(tc, line, &tc->pull_direction,
                                    TCAL9539_PUD0);
    }
    if(err)
      return err;

    return tcal9539_set_bit_in_reg(tc, line, &tc->pull_enable, TCAL9539_PE0);
  }
}


static error_t
tcal9539_conf_input(indirect_gpio_t *ig, unsigned int line, gpio_pull_t pull)
{
  tcal9539_t *tc = (tcal9539_t *)ig;
  if(line >= 16)
    return ERR_INVALID_ID;

  error_t err = tcal9539_conf_pull(tc, line, pull);
  if(err)
    return err;

  return tcal9539_set_bit_in_reg(tc, line, &tc->direction, TCAL9539_CFG0);
}


static error_t
tcal9539_conf_output(indirect_gpio_t *ig, unsigned int line,
                     gpio_output_type_t type, gpio_output_speed_t speed,
                     gpio_pull_t pull)
{
  tcal9539_t *tc = (tcal9539_t *)ig;

  if(line >= 16)
    return ERR_INVALID_ID;

  error_t err = tcal9539_conf_pull(tc, line, pull);
  if(err)
    return err;

  return tcal9539_clr_bit_in_reg(tc, line, &tc->direction, TCAL9539_CFG0);
}


static error_t
tcal9539_set_pin(indirect_gpio_t *ig, unsigned int line, int on)
{
  tcal9539_t *tc = (tcal9539_t *)ig;

  if(line >= 16)
    return ERR_INVALID_ID;

  if(on) {
    return tcal9539_set_bit_in_reg(tc, line, &tc->output, TCAL9539_OUTPUT0);
  } else {
    return tcal9539_clr_bit_in_reg(tc, line, &tc->output, TCAL9539_OUTPUT0);
  }

  return ERR_NOT_IMPLEMENTED;
}


static error_t
tcal9539_get_pin(indirect_gpio_t *ig, unsigned int line, int *status)
{
  tcal9539_t *tc = (tcal9539_t *)ig;

  if(line >= 16)
    return ERR_INVALID_ID;

  uint8_t val;
  error_t err = i2c_read_u8(tc->i2c, tc->address, line >> 3, &val);
  if(err)
    return err;

  int bit = line & 0x7;
  *status = !!(val & (1 << bit));
  return 0;
}

static error_t
tcal9539_get_port(indirect_gpio_t *ig, unsigned int port, uint32_t *status)
{
  tcal9539_t *tc = (tcal9539_t *)ig;

  if(port >= 2)
    return ERR_INVALID_ID;

  uint8_t val;
  error_t err = i2c_read_u8(tc->i2c, tc->address, port, &val);
  if(err)
    return err;

  *status = val;
  return 0;
}


static const gpio_vtable_t tcal9539_vtable = {
  .conf_input = tcal9539_conf_input,
  .conf_output = tcal9539_conf_output,
  .set_pin = tcal9539_set_pin,
  .get_pin = tcal9539_get_pin,
  .get_port = tcal9539_get_port,
};


indirect_gpio_t *
tcal9539_create(i2c_t *i2c, uint8_t address)
{
  tcal9539_t *tc = calloc(1, sizeof(tcal9539_t));

  tc->gpio.vtable = &tcal9539_vtable;
  tc->i2c = i2c;
  tc->address = address;
  tc->direction = 0xffff;
  tc->pull_direction = 0xffff;

  return &tc->gpio;
}
