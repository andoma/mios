#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"
#include "stm32wb_reg.h"
#include "stm32wb_clk.h"


#include "platform/stm32/stm32_i2c_v2.c"

static stm32_i2c_t *g_i2c[3];

i2c_t *
i2c_get_bus(unsigned int bus_id)
{
  bus_id--;
  if(bus_id > ARRAYSIZE(g_i2c))
    return NULL;
  return (i2c_t *)g_i2c[bus_id];
}

static const struct i2c_config {
  uint32_t base_addr;
  uint16_t clk_id;
  uint8_t irq_ev;
  uint8_t irq_er;

} i2c_configs[] = {
  { 0x40005400, CLK_I2C1, 30,31 },
  { 0, 0, 0 },
  { 0x40005c00, CLK_I2C3, 32,33 },
};

i2c_t *
stm32wb_i2c_create(unsigned int instance, gpio_t scl, gpio_t sda,
                   gpio_pull_t pull)
{
  instance--;
  if(instance > ARRAYSIZE(i2c_configs))
    return NULL;

  if(g_i2c[instance])
    return NULL;


  const struct i2c_config *c = &i2c_configs[instance];
  if(c->base_addr == 0)
    return NULL;

  gpio_conf_af(scl, 4, GPIO_OPEN_DRAIN, GPIO_SPEED_LOW, pull);
  gpio_conf_af(sda, 4, GPIO_OPEN_DRAIN, GPIO_SPEED_LOW, pull);

  clk_enable(c->clk_id);
  stm32_i2c_t *d = stm32_i2c_create(c->base_addr);
  irq_enable_fn_arg(c->irq_ev, IRQ_LEVEL_IO, i2c_irq, d);
  irq_enable_fn_arg(c->irq_er, IRQ_LEVEL_IO, i2c_irq, d);

  g_i2c[instance] = d;

  // 100kHz i2c
  int presc = clk_get_freq(c->clk_id) / 4000000 - 1;
  int scll = 0x13;
  int sclh = 0xf;
  int sdadel = 0x2;
  int scldel = 0x4;

  reg_wr(d->base_addr + I2C_TIMINGR,
         (presc << 28) |
         (scldel << 20) |
         (sdadel << 16) |
         (sclh << 8) |
         (scll));

  return &d->i2c;
}
