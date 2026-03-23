#include <mios/task.h>
#include <mios/type_macros.h>

#include "stm32h7_clk.h"
#include "stm32h7_tim.h"
#include "stm32h7_dma.h"
#include "stm32h7_pwg.h"

#include "irq.h"

#define GPIO_PORT_ADDR(x) (0x58020000 + ((x) * 0x400))

#include "platform/stm32/stm32_pwg.c"


static const struct {
  uint32_t base;
  uint16_t clkid;
  uint8_t dma_resource;
} pwg_timers[] = {
  [1] = { TIM1_BASE, CLK_TIM1, 15 },
  [2] = { TIM2_BASE, CLK_TIM2, 22 },
  [3] = { TIM3_BASE, CLK_TIM3, 27 },
  [4] = { TIM4_BASE, CLK_TIM4, 32 },
  [5] = { TIM5_BASE, CLK_TIM5, 59 },
  [6] = { TIM6_BASE, CLK_TIM6, 69 },
  [7] = { TIM7_BASE, CLK_TIM7, 70 },
  [8] = { TIM8_BASE, CLK_TIM8, 51 },
};


stm32_pwg_t *
stm32h7_pwg_create(unsigned int timer_instance,
                   uint8_t gpio_port,
                   uint32_t frequency,
                   pwg_fill_cb fill_cb,
                   void *opaque)
{
  if(timer_instance < 1 || timer_instance >= ARRAYSIZE(pwg_timers))
    return NULL;

  return stm32_pwg_init(pwg_timers[timer_instance].base,
                        pwg_timers[timer_instance].clkid,
                        pwg_timers[timer_instance].dma_resource,
                        GPIO_PORT_ADDR(gpio_port) + 0x18,
                        frequency,
                        fill_cb,
                        opaque);
}
