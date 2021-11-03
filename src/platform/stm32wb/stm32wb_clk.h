#pragma once

#include "stm32wb_reg.h"


#define RCC_BASE 0x58000000


#define RCC_AHB2ENR  (RCC_BASE + 0x4c)
#define RCC_APB1ENR2 (RCC_BASE + 0x5c)
#define RCC_APB2ENR  (RCC_BASE + 0x60)

#define CLK_ID(reg, bit) (((reg & 0xff) << 8) | (bit))

#define CLK_GPIO(x) CLK_ID(RCC_AHB2ENR, (x))

#define CLK_GPIOA CLK_GPIO(0)
#define CLK_GPIOB CLK_GPIO(1)
#define CLK_GPIOC CLK_GPIO(2)
#define CLK_GPIOE CLK_GPIO(4)
#define CLK_GPIOH CLK_GPIO(7)


#define CLK_USART1  CLK_ID(RCC_APB2ENR,  14)
#define CLK_LPUART1 CLK_ID(RCC_APB1ENR2, 14)

static inline void
clk_enable(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 8), id & 0xff);
}

static inline void
clk_disable(uint16_t id)
{
  reg_clr_bit(RCC_BASE + (id >> 8), id & 0xff);
}

int clk_get_freq(uint16_t id);
