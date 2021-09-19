#include "stm32g0_reg.h"
#include "stm32g0_clk.h"

void
reset_peripheral(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 8), id & 0xff);
  reg_clr_bit(RCC_BASE + (id >> 8), id & 0xff);
}


int
clk_get_freq(uint16_t id)
{
  return 16000000;
}
