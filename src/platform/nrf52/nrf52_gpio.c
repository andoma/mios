#include <mios/io.h>

#include "nrf52_reg.h"

#define GPIO_BASE 0x50000000

#define GPIO_PIN_CNF(x) (GPIO_BASE + 0x700 + (x) * 4)

#define GPIO_OUTSET (GPIO_BASE + 0x508)
#define GPIO_OUTCLR (GPIO_BASE + 0x50c)

static const uint8_t pullmap[3] = {0, 3, 1};

void
gpio_conf_output(gpio_t gpio, gpio_output_type_t type,
                 gpio_output_speed_t speed, gpio_pull_t pull)
{
  const uint32_t drive = 0;

  const uint32_t reg =
    (1 << 0) |                        // OUTPUT
    (0 << 1) |                        // Connect (0 = yes)
    (pullmap[pull]  << 2) |
    (drive << 8);

  reg_wr(GPIO_PIN_CNF(gpio), reg);
}


void
gpio_set_output(gpio_t gpio, int on)
{
  if(on)
    reg_wr(GPIO_OUTSET, 1 << gpio);
  else
    reg_wr(GPIO_OUTCLR, 1 << gpio);
}
