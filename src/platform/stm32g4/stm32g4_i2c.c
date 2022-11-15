#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"
#include "stm32g4_reg.h"
#include "stm32g4_clk.h"

#include "platform/stm32/stm32_i2c_v2.c"

static stm32_i2c_t *g_i2c[4];

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
  i2c_irq(g_i2c[0]);
}
void
irq_32(void)
{
  i2c_irq(g_i2c[0]);
}
void
irq_33(void)
{
  i2c_irq(g_i2c[1]);
}
void
irq_34(void)
{
  i2c_irq(g_i2c[1]);
}
void
irq_92(void)
{
  i2c_irq(g_i2c[2]);
}
void
irq_93(void)
{
  i2c_irq(g_i2c[2]);
}
void
irq_82(void)
{
  i2c_irq(g_i2c[3]);
}
void
irq_83(void)
{
  i2c_irq(g_i2c[3]);
}



static const struct i2c_config {
  uint32_t base_addr;
  uint16_t clk_id;
  uint8_t irq1;
  uint8_t irq2;

} i2c_configs[] = {
  { 0x40005400, CLK_I2C1, 31, 32 },
  { 0x40005800, CLK_I2C2, 33, 34 },
  { 0x40007800, CLK_I2C3, 92, 93 },
  { 0x40008400, CLK_I2C4, 82, 83 },
};

i2c_t *
stm32g4_i2c_create(unsigned int instance, gpio_t scl, gpio_t sda,
                   gpio_pull_t pull)
{
  instance--;
  if(instance > ARRAYSIZE(i2c_configs))
    return NULL;

  if(g_i2c[instance])
    return NULL;

  const struct i2c_config *c = &i2c_configs[instance];

  gpio_conf_af(scl, 4, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, pull);
  gpio_conf_af(sda, 4, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, pull);

  clk_enable(c->clk_id);
  stm32_i2c_t *d = stm32_i2c_create(c->base_addr);
  irq_enable(c->irq1, IRQ_LEVEL_IO);
  irq_enable(c->irq2, IRQ_LEVEL_IO);

  g_i2c[instance] = d;

  // NOT TRUE 16MHz input clock -> 100kHz i2c
  int presc = 5;
  int scll = 0x13;
  int sclh = 0xf;
  int sdadel = 0x2;
  int scldel = 0x4;

  printf("i2cclk: %d\n", clk_get_freq(c->clk_id));

  reg_wr(d->base_addr + I2C_TIMINGR,
         (presc << 28) |
         (scldel << 20) |
         (sdadel << 16) |
         (sclh << 8) |
         (scll));

  return &d->i2c;
}
