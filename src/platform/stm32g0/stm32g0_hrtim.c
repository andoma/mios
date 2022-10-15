#include "stm32g0_reg.h"
#include "stm32g0_tim.h"
#include "stm32g0_clk.h"

#include "platform/stm32/stm32_hrtim.c"

static void  __attribute__((constructor(131)))
stm32g0_tim_init(void)
{
  hrtimer_init(TIM7_BASE, CLK_TIM7, 18);
}
