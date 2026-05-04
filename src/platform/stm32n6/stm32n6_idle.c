#include "stm32n6_reg.h"
#include "stm32n6_wdog.h"

void __attribute__((noreturn))
cpu_idle(void)
{
  while(1) {
    asm volatile("wfi");
    reg_wr(IWDG_KR, 0xAAAA);
  }
}
