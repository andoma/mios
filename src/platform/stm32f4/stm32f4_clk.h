#pragma once

#include <stm32f4.h>

#define CLK_AHB1 0x30
#define CLK_AHB2 0x34
#define CLK_APB1 0x40
#define CLK_APB2 0x44

#define RCC_BASE 0x40023800

#define CLK_ID(reg, bit) (((reg) << 8) | (bit))

#define CLK_SPI1 CLK_ID(CLK_APB2, 12)
#define CLK_SPI2 CLK_ID(CLK_APB1, 14)
#define CLK_SPI3 CLK_ID(CLK_APB1, 15)

#define CLK_USART1 CLK_ID(CLK_APB2, 4)
#define CLK_USART2 CLK_ID(CLK_APB1, 17)
#define CLK_USART3 CLK_ID(CLK_APB1, 18)
#define CLK_UART4  CLK_ID(CLK_APB1, 19)
#define CLK_UART5  CLK_ID(CLK_APB1, 20)
#define CLK_USART6 CLK_ID(CLK_APB2, 5)

#define CLK_TIM2  CLK_ID(CLK_APB1, 0)
#define CLK_TIM3  CLK_ID(CLK_APB1, 1)
#define CLK_TIM4  CLK_ID(CLK_APB1, 2)
#define CLK_TIM5  CLK_ID(CLK_APB1, 3)
#define CLK_TIM6  CLK_ID(CLK_APB1, 4)
#define CLK_TIM7  CLK_ID(CLK_APB1, 5)
#define CLK_TIM12 CLK_ID(CLK_APB1, 6)
#define CLK_TIM13 CLK_ID(CLK_APB1, 7)
#define CLK_TIM14 CLK_ID(CLK_APB1, 8)

#define CLK_CCMDATARAMEN CLK_ID(CLK_AHB1, 20)
#define CLK_RNG          CLK_ID(CLK_AHB2, 6)


static inline void
clk_enable(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 8), id & 0xff);
}

int clk_get_freq(uint16_t id);
