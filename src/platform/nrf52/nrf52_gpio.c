#include <mios/io.h>

#include "nrf52_reg.h"

#include "irq.h"

#include <stdio.h>

#define GPIO_BASE 0x50000000

#define GPIO_PIN_CNF(x) (GPIO_BASE + 0x700 + (x) * 4)

#define GPIO_OUTSET (GPIO_BASE + 0x508)
#define GPIO_OUTCLR (GPIO_BASE + 0x50c)
#define GPIO_IN     (GPIO_BASE + 0x510)

static const uint8_t pullmap[3] = {0, 3, 1};

void
gpio_disconnect(gpio_t gpio)
{
  reg_wr(GPIO_PIN_CNF(gpio), 2); // Set disconnect BIT
}


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
gpio_conf_input(gpio_t gpio, gpio_pull_t pull)
{
  const uint32_t reg =
    (0 << 0) |                        // INPUT
    (0 << 1) |                        // Connect INPUT (0 = yes)
    (pullmap[pull]  << 2);
  reg_wr(GPIO_PIN_CNF(gpio), reg);
}


int
gpio_get_input(gpio_t gpio)
{
  return (reg_rd(GPIO_IN) >> gpio) & 1;
}

void
gpio_set_output(gpio_t gpio, int on)
{
  reg_wr(on ? GPIO_OUTSET : GPIO_OUTCLR, 1 << gpio);
}



typedef struct {
  void (*cb)(void *arg);
  void *arg;
} gpiote_event_t;

static uint8_t gpiote_level = 0xff;

static gpiote_event_t gpiote_events[8];

#define GPIOTE_BASE       0x40006000

#define GPIOTE_EVENTS_IN(x) (GPIOTE_BASE + 0x100 + (x) * 4)
#define GPIOTE_CONFIG(x)    (GPIOTE_BASE + 0x510 + (x) * 4)
#define GPIOTE_INTENSET     (GPIOTE_BASE + 0x304)
#define GPIOTE_INTENCLR     (GPIOTE_BASE + 0x308)

// GPIOTE
void
irq_6(void)
{
  for(int i = 0; i < 8; i++) {
    if(reg_rd(GPIOTE_EVENTS_IN(i))) {
      reg_wr(GPIOTE_EVENTS_IN(i), 0);
      gpiote_events[i].cb(gpiote_events[i].arg);
    }
  }
}


void
gpio_conf_irq(gpio_t gpio, gpio_pull_t pull,
              void (*cb)(void *arg), void *arg,
              gpio_edge_t edge, int level)
{
  if(level < gpiote_level) {
    gpiote_level = level;
    irq_enable(6, level);
  }

  gpio_conf_input(gpio, pull);

  for(int i = 0; i < ARRAYSIZE(gpiote_events); i++) {
    if(gpiote_events[i].cb)
      continue;

    uint32_t config = 1; // MODE=EVENT

    config |= gpio << 8;

    if(edge & GPIO_FALLING_EDGE)
      config |= (1 << 17);
    if(edge & GPIO_RISING_EDGE)
      config |= (1 << 16);
    reg_wr(GPIOTE_CONFIG(i), config);
    gpiote_events[i].arg = arg;
    gpiote_events[i].cb = cb;
    reg_wr(GPIOTE_INTENSET, (1 << i));
    return;
  }
  panic("Out of GPIOTE event slots");
}
