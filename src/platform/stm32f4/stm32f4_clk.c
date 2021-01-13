#include <mios/mios.h>

#include "stm32f4_clk.h"
#include "cpu.h"
#include "systick.h"

int
clk_get_freq(uint16_t id)
{
  switch(id >> 8) {
  default:
    panic("clk_get_speed() invalid id: 0x%x", id);
  case CLK_APB1:
    return STM32F4_APB1CLOCK;
  case CLK_APB2:
    return STM32F4_APB2CLOCK;
  }
}


static volatile unsigned int * const SYST_RVR = (unsigned int *)0xe000e014;

void
systick_timepulse(void)
{
  uint32_t c = cpu_cycle_counter();
  static uint32_t prev;
  uint32_t delta = c - prev;
  prev = c;

  static uint32_t lp;

  if(delta > CPU_SYSTICK_RVR - 10000 && delta < CPU_SYSTICK_RVR + 10000) {
    if(lp) {
      lp = (lp * 3 + 2 + delta) / 4;
      *SYST_RVR = (lp + (HZ / 2)) / HZ - 1;
    } else {
      lp = delta;
    }
  } else {
    lp = 0;
  }
}
