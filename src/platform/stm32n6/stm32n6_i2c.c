#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/type_macros.h>
#include <mios/task.h>
#include <mios/eventlog.h>

#include "irq.h"
#include "stm32n6_clk.h"
#include "stm32n6_i2c.h"

#include "platform/stm32/stm32_i2c_v2.c"
#include "platform/stm32/stm32_i2c_v2_timings.h"


static const struct i2c_config {
  uint32_t base_addr;
  uint16_t clk_id;
  uint8_t irq1;
  uint8_t irq2;
} i2c_configs[] = {
  { 0x50005400, CLK_I2C1, 100, 101 },
  { 0x50005800, CLK_I2C2, 102, 103 },
  { 0x50005c00, CLK_I2C3, 104, 105 },
  { 0x56001c00, CLK_I2C4, 106, 107 },
};


i2c_t *
stm32n6_i2c_create_unit(unsigned int instance, int scl_freq)
{
  instance--;
  if(instance >= ARRAYSIZE(i2c_configs))
    panic("i2c: Invalid instance %d", instance + 1);

  const struct i2c_config *c = &i2c_configs[instance];

  int timingr = stm32_i2c_calc_timingr(clk_get_freq(c->clk_id),
                                       scl_freq, 1, 0);
  if(timingr == -1)
    panic("i2c-%d: Unsupported timing", instance + 1);

  clk_enable(c->clk_id);

  stm32_i2c_t *d = stm32_i2c_create(c->base_addr);

  irq_enable_fn_arg(c->irq1, IRQ_LEVEL_IO, i2c_irq, d);
  irq_enable_fn_arg(c->irq2, IRQ_LEVEL_IO, i2c_irq, d);

  reg_wr(d->base_addr + I2C_TIMINGR, timingr);
  return &d->i2c;
}


