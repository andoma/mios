#include "stm32g0_reg.h"
#include "stm32g0_tim.h"
#include "stm32g0_clk.h"

#include "platform/stm32/stm32_systim.c"

static void  __attribute__((constructor(131)))
stm32g0_tim_init(void)
{
  if(stm32_systim_init(TIM7_BASE, CLK_TIM7, 18)) {
    if(stm32_systim_init(TIM17_BASE, CLK_TIM17, 22)) {
      printf("No system timer\n");
    } else {
      printf("systimer: TIM17\n");
    }
  } else {
    printf("systimer: TIM7\n");
  }
}
