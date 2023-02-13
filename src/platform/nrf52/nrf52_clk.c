#include "nrf52_clk.h"
#include "nrf52_reg.h"


#define CLOCK_BASE 0x40000000

#define CLOCK_TASKS_HFCLKSTART    (CLOCK_BASE + 0x000)
#define CLOCK_EVENTS_HFCLKSTARTED (CLOCK_BASE + 0x100)

void
nrf52_xtal_enable(void)
{
  reg_wr(CLOCK_TASKS_HFCLKSTART, 1);
  while(reg_rd(CLOCK_EVENTS_HFCLKSTARTED) == 0) {}
}
