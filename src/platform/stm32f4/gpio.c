#include "reg.h"
#include "stm32f4.h"
#include "gpio.h"


void
gpio_conf_input(int port, int bit, gpio_pull_t pull)
{
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 0);
  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
}



void
gpio_conf_output(int port, int bit,
                 gpio_output_type_t type,
                 gpio_output_speed_t speed,
                 gpio_pull_t pull)
{
  reg_set_bits(GPIO_OTYPER(port),  bit, 1, type);
  reg_set_bits(GPIO_OSPEEDR(port), bit * 2, 2, speed);
  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);
  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 1);
}


void
gpio_conf_af(int port, int bit, int af, gpio_output_speed_t speed,
             gpio_pull_t pull)
{
  reg_set_bits(GPIO_OSPEEDR(port), bit * 2, 2, speed);

  if(bit < 8) {
    reg_set_bits(GPIO_AFRL(port), bit * 4, 4, af);
  } else {
    reg_set_bits(GPIO_AFRH(port), (bit - 8) * 4, 4, af);
  }

  reg_set_bits(GPIO_PUPDR(port), bit * 2, 2, pull);

  reg_set_bits(GPIO_MODER(port), bit * 2, 2, 2);
}



void
gpio_set_output(int port, int bit, int on)
{
  reg_set(GPIO_BSRR(port), 1 << (bit + !on * 16));
}
