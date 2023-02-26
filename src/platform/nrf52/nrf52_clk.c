#include "nrf52_clk.h"
#include "nrf52_reg.h"



void
nrf52_xtal_enable(void)
{
  reg_wr(CLOCK_TASKS_HFCLKSTART, 1);
  while(reg_rd(CLOCK_EVENTS_HFCLKSTARTED) == 0) {}
}

void
nrf52_lfclk_enable(void)
{
  reg_wr(CLOCK_TASKS_LFCLKSTART, 1);
  while(reg_rd(CLOCK_EVENTS_LFCLKSTARTED) == 0) {}
}

