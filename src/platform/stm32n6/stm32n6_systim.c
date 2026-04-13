#include "stm32n6_reg.h"
#include "stm32n6_tim.h"
#include "stm32n6_clk.h"

#include "platform/stm32/stm32_systim.c"

static void  __attribute__((constructor(190)))
stm32n6_tim_init(void)
{
  stm32_systim_init(TIM7_BASE, CLK_TIM7, 121);
}
