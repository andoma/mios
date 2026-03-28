#pragma once

#include <mios/io.h>

i2c_t *stm32n6_i2c_create_unit(unsigned int instance, int scl_freq);

extern int stm32n6_i2c_invalid_af_for_pin; // never defined — link error on bad pin

static inline int __attribute__((always_inline))
stm32n6_i2c_pin_af(unsigned int instance, gpio_t pin)
{
  switch(instance) {
  case 1:
    switch(pin) {
    case GPIO_PE(5):              // SCL
    case GPIO_PE(6):              // SDA
    case GPIO_PH(9):              // SCL
      return 4;
    }
    break;
  case 2:
    switch(pin) {
    case GPIO_PB(10):             // SCL
    case GPIO_PB(11):             // SDA
    case GPIO_PB(12):             // SMBA
    case GPIO_PD(14):             // SCL
    case GPIO_PD(15):             // SDA
      return 4;
    }
    break;
  case 3:
    switch(pin) {
    case GPIO_PA(8):              // SCL
    case GPIO_PA(9):              // SDA
    case GPIO_PC(8):              // SMBA
    case GPIO_PC(9):              // SDA
    case GPIO_PH(7):              // SCL
    case GPIO_PH(8):              // SDA
      return 4;
    }
    break;
  case 4:
    switch(pin) {
    case GPIO_PC(10):             // SCL
    case GPIO_PC(11):             // SDA
    case GPIO_PD(11):             // SDA
    case GPIO_PE(13):             // SCL
    case GPIO_PE(14):             // SDA
    case GPIO_PE(15):             // SMBA
      return 4;
    }
    break;
  }
  return stm32n6_i2c_invalid_af_for_pin;
}

static inline i2c_t * __attribute__((always_inline))
stm32n6_i2c_create(unsigned int instance, gpio_t scl, gpio_t sda,
                    gpio_pull_t pull, int scl_freq)
{
  gpio_conf_af(scl, stm32n6_i2c_pin_af(instance, scl),
               GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, pull);
  gpio_conf_af(sda, stm32n6_i2c_pin_af(instance, sda),
               GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, pull);
  return stm32n6_i2c_create_unit(instance, scl_freq);
}
