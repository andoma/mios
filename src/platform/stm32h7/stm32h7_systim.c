#include "stm32h7_reg.h"
#include "stm32h7_tim.h"
#include "stm32h7_clk.h"

#include "platform/stm32/stm32_systim.c"

static void  __attribute__((constructor(190)))
stm32h7_tim_init(void)
{
  stm32_systim_init(TIM7_BASE, CLK_TIM7, 55);
}
