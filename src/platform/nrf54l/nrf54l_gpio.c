#include "nrf54l_gpio.h"

#include <mios/io.h>

#include "nrf54l_reg.h"

// PIN_CNF: DIR[0], INPUT[1] (0=connect buffer), PULL[3:2], DRIVE[12:8],
//          CTRLSEL[30:28] (0 = GPIO controlled by CPU).
// PULL field: 0=disabled, 1=pulldown, 3=pullup. mios pull enum order is
// {NONE, UP, DOWN} -> {0, 3, 1}.
static const uint8_t pullmap[3] = {0, 3, 1};


void
gpio_disconnect(gpio_t gpio)
{
  reg_wr(GPIO_PIN_CNF(gpio), 1 << 1); // INPUT disconnected, DIR input
}


void
gpio_conf_output(gpio_t gpio, gpio_output_type_t type,
                 gpio_output_speed_t speed, gpio_pull_t pull)
{
  const uint32_t reg =
    (1 << 0) |                  // DIR = output
    (pullmap[pull] << 2);

  reg_wr(GPIO_PIN_CNF(gpio), reg);
}


void
gpio_conf_input(gpio_t gpio, gpio_pull_t pull)
{
  const uint32_t reg =
    (0 << 0) |                  // DIR = input
    (0 << 1) |                  // INPUT buffer connected
    (pullmap[pull] << 2);

  reg_wr(GPIO_PIN_CNF(gpio), reg);
}


int
gpio_get_input(gpio_t gpio)
{
  return (reg_rd(GPIO_IN(gpio)) >> GPIO_PIN(gpio)) & 1;
}


void
gpio_set_output(gpio_t gpio, int on)
{
  reg_wr(on ? GPIO_OUTSET(gpio) : GPIO_OUTCLR(gpio), 1 << GPIO_PIN(gpio));
}
