#pragma once

#include "stm32g0.h"


#define RCC_BASE 0x40021000

#define RCC_IOPENR  0x34
#define RCC_AHBENR  0x38
#define RCC_APBENR1 0x3c
#define RCC_APBENR2 0x40

#define CLK_ID(reg, bit) (((reg) << 8) | (bit))


#define CLK_GPIOA CLK_ID(RCC_IOPENR, 0)
#define CLK_GPIOB CLK_ID(RCC_IOPENR, 1)
#define CLK_GPIOC CLK_ID(RCC_IOPENR, 2)
#define CLK_GPIOD CLK_ID(RCC_IOPENR, 3)
#define CLK_GPIOE CLK_ID(RCC_IOPENR, 4)
#define CLK_GPIOF CLK_ID(RCC_IOPENR, 5)

#define CLK_USART1 CLK_ID(RCC_APBENR2, 14)
#define CLK_USART2 CLK_ID(RCC_APBENR1, 17)
#define CLK_USART3 CLK_ID(RCC_APBENR1, 18)
#define CLK_USART4 CLK_ID(RCC_APBENR1, 19)
#define CLK_USART5 CLK_ID(RCC_APBENR1,  8)
#define CLK_USART6 CLK_ID(RCC_APBENR1,  9)


static inline void
clk_enable(uint16_t id)
{
  reg_set_bits(RCC_BASE + (id >> 8), id & 0xff, 1, 1);
}

int clk_get_freq(uint16_t id);
