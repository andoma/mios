#include "stm32f4_reg.h"
#include "stm32f4_tim.h"
#include "stm32f4_clk.h"

#include "platform/stm32/stm32_systim.c"

static void  __attribute__((constructor(131)))
stm32f4_tim_init(void)
{
  const char *where = "NONE";
  if(stm32_systim_init(TIM7_BASE, CLK_TIM7, 55)) {
    if(!stm32_systim_init(TIM11_BASE, CLK_TIM11, 26)) {
      where = "tim11";
    }
  } else {
    where = "tim7";
  }
  printf("systim: %s\n", where);
}
