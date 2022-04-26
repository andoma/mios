#ifdef HAVE_HRTIMER

#include "stm32g0_reg.h"
#include "stm32g0_tim.h"
#include "stm32g0_clk.h"


#define HRTIM_BASE TIM7_BASE

#include "platform/stm32/stm32_hrtim.c"

static void  __attribute__((constructor(131)))
stm32g0_tim_init(void)
{
  hrtimer_init(CLK_TIM7);
  irq_enable(18, IRQ_LEVEL_CLOCK);
}

void
irq_18(void)
{
  hrtim_irq();
}

#endif
