#include "irq.h"

#include "stm32h7_wdog.h"
#include "stm32h7_reg.h"

void __attribute__((noreturn))
cpu_idle(void)
{
  while(1) {
    asm volatile("wfi");
    reg_wr(IWDG_KR, 0xAAAA);
    (void)reg_rd(LPTIM5_CNT);  // RSTARE=1 → read resets counter
  }
}
