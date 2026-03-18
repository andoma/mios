#pragma once

#include <mios/io.h>

i2c_t *stm32h7_i2c_create_unit(unsigned int instance, int scl_freq);

extern int stm32h7_i2c_invalid_af_for_pin; // never defined — link error on bad pin

static inline int __attribute__((always_inline))
stm32h7_i2c_pin_af(unsigned int instance, gpio_t pin)
{
  switch(instance) {
  case 1:
    switch(pin) {
    case GPIO_PB(5): case GPIO_PB(6): case GPIO_PB(7):
    case GPIO_PB(8): case GPIO_PB(9):
      return 4;
    }
    break;
  case 2:
    switch(pin) {
    case GPIO_PB(10): case GPIO_PB(11): case GPIO_PB(12):
    case GPIO_PF(0):  case GPIO_PF(1):
    case GPIO_PH(4):  case GPIO_PH(5):
      return 4;
    }
    break;
  case 3:
    switch(pin) {
    case GPIO_PA(8):
    case GPIO_PC(9):
    case GPIO_PH(7): case GPIO_PH(8):
      return 4;
    }
    break;
  case 4:
    switch(pin) {
    case GPIO_PB(6): case GPIO_PB(7):
    case GPIO_PB(8): case GPIO_PB(9):
      return 6;
    case GPIO_PD(12): case GPIO_PD(13):
    case GPIO_PF(14): case GPIO_PF(15):
    case GPIO_PH(11): case GPIO_PH(12):
      return 4;
    }
    break;
  }
  return stm32h7_i2c_invalid_af_for_pin;
}

static inline i2c_t * __attribute__((always_inline))
stm32h7_i2c_create(unsigned int instance, gpio_t scl, gpio_t sda,
                   gpio_pull_t pull, int scl_freq)
{
  gpio_conf_af(scl, stm32h7_i2c_pin_af(instance, scl),
               GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, pull);
  gpio_conf_af(sda, stm32h7_i2c_pin_af(instance, sda),
               GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, pull);
  return stm32h7_i2c_create_unit(instance, scl_freq);
}
