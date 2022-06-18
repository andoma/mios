#include "stm32f4_reg.h"
#include "stm32f4_tim.h"
#include "stm32f4_clk.h"

#define HRTIM_BASE TIM7_BASE

#include "platform/stm32/stm32_hrtim.c"

static void  __attribute__((constructor(131)))
stm32f4_tim_init(void)
{
  hrtimer_init(CLK_TIM7);
  irq_enable(55, IRQ_LEVEL_CLOCK);
}

void
irq_55(void)
{
  hrtim_irq();
}
