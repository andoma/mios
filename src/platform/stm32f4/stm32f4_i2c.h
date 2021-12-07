#pragma once

#include <mios/io.h>

i2c_t *stm32f4_i2c_create(int instance, uint32_t scl, uint32_t sda,
                          gpio_pull_t pull);

static inline uint32_t stm32f4_i2c_sda(int instance, gpio_t sda)
{
  if(instance == 1)
    return (4 << 8) | sda;
  if(instance == 2 && sda == GPIO_PB(3))
    return (9 << 8) | sda;
  if(instance == 2 && sda == GPIO_PB(9))
    return (9 << 8) | sda;
  if(instance == 2 && sda == GPIO_PB(11))
    return (4 << 8) | sda;
  if(instance == 3 && sda == GPIO_PC(9))
    return (4 << 8) | sda;
  if(instance == 3 && sda == GPIO_PB(4))
    return (9 << 8) | sda;
  if(instance == 3 && sda == GPIO_PB(8))
    return (9 << 8) | sda;

  extern void invalid_i2c_sda_pin(void);
  invalid_i2c_sda_pin();
  return 0;
}
