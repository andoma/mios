#include "stm32g4_reg.h"
#include "stm32g4_tim.h"
#include "stm32g4_clk.h"

#include "platform/stm32/stm32_systim.c"

static void  __attribute__((constructor(131)))
stm32g4_tim_init(void)
{
  stm32_systim_init(TIM7_BASE, CLK_TIM7, 55);
}
