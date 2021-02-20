#include "stm32h7_clk.h"
#include "stm32h7.h"


#define STM32H7_RCC_BASE 0x58024400

#define RCC_CFGR (STM32H7_RCC_BASE + 0x10)

void
stm32h7_init_pll(void)
{
}



unsigned int
clk_get_freq(uint16_t id)
{
  return CPU_SYSTICK_RVR;
}
