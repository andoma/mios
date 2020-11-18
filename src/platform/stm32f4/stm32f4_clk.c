#include "mios.h"
#include "stm32f4_clk.h"
#include "clk_config.h"

int
clk_get_freq(uint16_t id)
{
  switch(id >> 8) {
  default:
    panic("clk_get_speed() invalid id: 0x%x", id);
  case CLK_APB1:
    return APB1CLOCK;
  case CLK_APB2:
    return APB2CLOCK;
  }
}
