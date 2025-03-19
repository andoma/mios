#include "irq.h"

#include "stm32h7_wdog.h"
#include "stm32h7_reg.h"

#define WDOG_TIMEOUT_SEC 3
#define WDOG_HZ 128

static void
wdog_init(void)
{
  reg_wr(IWDG_KR, 0x5555);
  reg_wr(IWDG_RLR, WDOG_HZ * WDOG_TIMEOUT_SEC);
  reg_wr(IWDG_PR, 6);  // Prescaler (/256) for 32768 -> WDOG_HZ
  reg_wr(IWDG_KR, 0xAAAA);
  reg_wr(IWDG_KR, 0xCCCC);
}

void  __attribute__((noreturn))
cpu_idle(void)
{
  wdog_init();

  while(1) {
    asm volatile("wfi");
    reg_wr(IWDG_KR, 0xAAAA);
  }
}
