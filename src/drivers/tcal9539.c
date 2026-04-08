// TCAL9539-Q1 Automotive Low-Voltage 16-Bit I2C-Bus, SMBus I/O Expander

// https://www.ti.com/lit/ds/symlink/tcal9539-q1.pdf

#include "tcal9539.h"
#include <mios/io.h>
#include <mios/task.h>
#include <mios/eventlog.h>
#include <stdlib.h>

#include "irq.h"

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
#define TCAL9539_IRQMSK0 0x4A
#define TCAL9539_IRQMSK1 0x4B



typedef struct {
  xgpio_t gpio;
  i2c_t *i2c;
  struct xgpio_irq_mux *irq_mux;

  uint8_t address;
  uint16_t output;
  uint16_t irq_mask;
  uint16_t direction;  // 1 = input (default)
  uint16_t pull_enable;
  uint16_t pull_direction;

  uint16_t input_shadow;
  uint16_t input_prev; // For edge detection

  uint16_t irq_mask_rising_edge;
  uint16_t irq_mask_falling_edge;

  struct {
    void (*cb)(void *arg);
    void *arg;
  } pin_irq[16];
} tcal9539_t;


static error_t
tcal9539_write_shadow(tcal9539_t *tc, unsigned int line,
                      uint16_t shadow, int reg)
{
  if(line < 8) {
    return i2c_write_u8(tc->i2c, tc->address, reg, shadow & 0xff);
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
tcal9539_conf_input(xgpio_t *ig, unsigned int line, gpio_pull_t pull)
{
  tcal9539_t *tc = (tcal9539_t *)ig;
  if(line >= 16)
    return ERR_INVALID_ID;

  error_t err = tcal9539_conf_pull(tc, line, pull);
  if(err)
    return err;

  err = tcal9539_set_bit_in_reg(tc, line, &tc->direction, TCAL9539_CFG0);
  if(err)
    return err;

  if(tc->irq_mux == NULL)
    return 0;

  err = tcal9539_clr_bit_in_reg(tc, line, &tc->irq_mask, TCAL9539_IRQMSK0);
  if(err)
    return err;

  xgpio_irq_mux_wakeup(tc->irq_mux);
  return 0;
}


static error_t
tcal9539_set_pin(xgpio_t *ig, unsigned int line, int on)
{
  tcal9539_t *tc = (tcal9539_t *)ig;

  if(line >= 16)
    return ERR_INVALID_ID;

  if(on) {
    return tcal9539_set_bit_in_reg(tc, line, &tc->output, TCAL9539_OUTPUT0);
  } else {
    return tcal9539_clr_bit_in_reg(tc, line, &tc->output, TCAL9539_OUTPUT0);
  }
}

static error_t
tcal9539_conf_irq(xgpio_t *ig, unsigned int line,
                  gpio_pull_t pull,
                  void (*cb)(void *arg), void *arg,
                  gpio_edge_t edge, int level)
{
  tcal9539_t *tc = (tcal9539_t *)ig;

  if(tc->irq_mux == NULL)
    return ERR_BAD_CONFIG;

  tc->pin_irq[line].cb = cb;
  tc->pin_irq[line].arg = arg;

  if(edge & GPIO_RISING_EDGE)
    tc->irq_mask_rising_edge |= 1 << line;

  if(edge & GPIO_FALLING_EDGE)
    tc->irq_mask_falling_edge |= 1 << line;

  return tcal9539_conf_input(ig, line, pull);
}


static error_t
tcal9539_conf_output(xgpio_t *ig, unsigned int line,
                     gpio_output_type_t type, gpio_output_speed_t speed,
                     gpio_pull_t pull, int initial_value)
{
  tcal9539_t *tc = (tcal9539_t *)ig;
  error_t err;

  if(line >= 16)
    return ERR_INVALID_ID;

  if(tc->irq_mux) {
    // Disable IRQ if any
    err = tcal9539_set_bit_in_reg(tc, line, &tc->irq_mask, TCAL9539_IRQMSK0);
    if(err)
      return err;
  }

  err = tcal9539_conf_pull(tc, line, pull);
  if(err)
    return err;

  if(initial_value != -1) {
    err = tcal9539_set_pin(ig, line, initial_value);
    if(err)
      return err;
  }

  return tcal9539_clr_bit_in_reg(tc, line, &tc->direction, TCAL9539_CFG0);
}


static error_t
tcal9539_get_pin(xgpio_t *ig, unsigned int line, int *status)
{
  tcal9539_t *tc = (tcal9539_t *)ig;

  if(line >= 16)
    return ERR_INVALID_ID;

  if((1 << line) & tc->direction) {

    if(tc->irq_mux != NULL) {
      *status = (tc->input_shadow >> line) & 1;
      return 0;
    }

    uint8_t val;
    error_t err = i2c_read_u8(tc->i2c, tc->address, line >> 3, &val);
    if(err)
      return err;

    int bit = line & 0x7;
    *status = !!(val & (1 << bit));
  } else {
    *status = (tc->output >> line) & 1;
  }

  return 0;
}

static int
tcal9539_get_mode(xgpio_t *ig, unsigned int line)
{
  tcal9539_t *tc = (tcal9539_t *)ig;
  return (tc->direction >> line) & 1;
}





static error_t
tcal9539_read_u16(tcal9539_t *tc, uint8_t reg, uint16_t *value)
{
  uint8_t lo, hi;
  error_t err;

  err = i2c_read_u8(tc->i2c, tc->address, reg, &lo);
  if(err)
    return err;
  err = i2c_read_u8(tc->i2c, tc->address, reg + 1, &hi);
  if(err)
    return err;

  *value = ((uint16_t)hi << 8) | lo;
  return 0;
}



static void
tcal9539_irq(xgpio_t *ig)
{
  tcal9539_t *tc = (tcal9539_t *)ig;

  if(tcal9539_read_u16(tc, TCAL9539_INPUT0, &tc->input_shadow))
    return;

  uint16_t both_edges = tc->irq_mask_rising_edge | tc->irq_mask_falling_edge;
  uint16_t changed = (tc->input_shadow ^ tc->input_prev) & both_edges;

  tc->input_prev = tc->input_shadow;

  while(changed) {
    const int pin = __builtin_ctz(changed);
    const int mask = 1 << pin;
    changed &= ~mask;

    const int new_val = tc->input_shadow & mask;

    if((new_val && (mask & tc->irq_mask_rising_edge)) ||
       (!new_val && (mask & tc->irq_mask_falling_edge))) {
      tc->pin_irq[pin].cb(tc->pin_irq[pin].arg);
    }
  }
}


static const xgpio_vtable_t tcal9539_vtable = {
  .conf_input = tcal9539_conf_input,
  .conf_output = tcal9539_conf_output,
  .set = tcal9539_set_pin,
  .get = tcal9539_get_pin,
  .get_mode = tcal9539_get_mode,
  .conf_irq = tcal9539_conf_irq,
  .irq = tcal9539_irq,
};


xgpio_t *
tcal9539_create(i2c_t *i2c, uint8_t address, struct xgpio_irq_mux *m)
{
  tcal9539_t *tc = calloc(1, sizeof(tcal9539_t));

  tc->gpio.vtable = &tcal9539_vtable;
  tc->i2c = i2c;
  tc->address = address;
  tc->irq_mask = 0xffff;

  // Read hardware state into shadow registers
  if(tcal9539_read_u16(tc, TCAL9539_OUTPUT0, &tc->output) ||
     tcal9539_read_u16(tc, TCAL9539_CFG0, &tc->direction) ||
     tcal9539_read_u16(tc, TCAL9539_PUD0, &tc->pull_direction) ||
     tcal9539_read_u16(tc, TCAL9539_PE0, &tc->pull_enable)) {
    free(tc);
    return NULL;
  }

  if(m != NULL) {
    if(tcal9539_read_u16(tc, TCAL9539_INPUT0, &tc->input_shadow)) {
      free(tc);
      return NULL;
    }
    xgpio_irq_mux_link(m, &tc->gpio);
    tc->irq_mux = m;
  }

  return &tc->gpio;
}
