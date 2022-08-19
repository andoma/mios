#include <mios/io.h>

#include "irq.h"
#include "stm32f4_reg.h"
#include "stm32f4_clk.h"

#include "platform/stm32/stm32_i2c_v1.c"

#define I2C_BASE(x)   (0x40005400 + ((x) * 0x400))

static stm32_i2c_t *g_i2c[3];

i2c_t *
i2c_get_bus(unsigned int bus_id)
{
  bus_id--;
  if(bus_id > ARRAYSIZE(g_i2c))
    return NULL;
  return (i2c_t *)g_i2c[bus_id];
}

void
irq_31(void)
{
  i2c_irq_ev(g_i2c[0]);
}

void
irq_32(void)
{
  i2c_irq_er(g_i2c[0]);
}

void
irq_33(void)
{
  i2c_irq_ev(g_i2c[1]);
}

void
irq_34(void)
{
  i2c_irq_er(g_i2c[1]);
}

void
irq_72(void)
{
  i2c_irq_ev(g_i2c[2]);
}

void
irq_73(void)
{
  i2c_irq_er(g_i2c[2]);
}

i2c_t *
stm32f4_i2c_create(int instance, gpio_t scl, uint32_t sda_cfg, gpio_pull_t pull)
{
  if(instance < 1 || instance > 3) {
    panic("i2c: Invalid instance %d", instance);
  }

  const int sda = sda_cfg & 0xff;
  const int sda_af = sda_cfg >> 8;
  if(!sda_af)
    panic("i2c: Bad sda config");

  instance--;


  // If bus seems to be stuck, toggle SCL until SDA goes high again
  gpio_conf_input(sda, GPIO_PULL_UP);
  gpio_conf_output(scl, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  int c = 0;
  while(1) {
    int d = gpio_get_input(sda);
    if(d)
      break;

    c = !c;
    gpio_set_output(scl, c);

    udelay(10);
  }

  const uint16_t clkid = CLK_I2C(instance);
  const uint16_t rstid = RST_I2C(instance);
  clk_enable(clkid);

  gpio_conf_af(scl, 4, GPIO_OPEN_DRAIN, GPIO_SPEED_VERY_HIGH, pull);
  gpio_conf_af(sda, sda_af, GPIO_OPEN_DRAIN, GPIO_SPEED_VERY_HIGH, pull);

  stm32_i2c_t *i2c = stm32_i2c_create(I2C_BASE(instance), clkid, rstid);

  uint8_t irqbase = (const uint8_t []){31,33,72}[instance];
  irq_enable(irqbase, IRQ_LEVEL_IO);
  irq_enable(irqbase + 1, IRQ_LEVEL_IO);
  g_i2c[instance] = i2c;
  return &i2c->i2c;
}
