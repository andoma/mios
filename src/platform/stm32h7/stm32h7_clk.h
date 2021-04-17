#pragma once

#include "stm32h7.h"

#define STM32H7_RCC_BASE 0x58024400

#define RCC_APB1LRSTR     0x90

#define STM32H7_CLK_AHB1  0xd8
#define STM32H7_CLK_AHB4  0xe0
#define STM32H7_CLK_APB1L 0xe8
#define STM32H7_CLK_APB2  0xf0
#define STM32H7_CLK_APB4  0xf4

#define CLK_ID(reg, bit) (((reg) << 8) | (bit))

#define CLK_SYSCFG CLK_ID(STM32H7_CLK_APB4, 1)

#define CLK_ETH1MACEN CLK_ID(STM32H7_CLK_AHB1, 15)
#define CLK_ETH1RXEN  CLK_ID(STM32H7_CLK_AHB1, 16)
#define CLK_ETH1TXEN  CLK_ID(STM32H7_CLK_AHB1, 17)

#define CLK_GPIO(x) CLK_ID(STM32H7_CLK_AHB4, (x))

#define CLK_GPIOA CLK_GPIO(0)
#define CLK_GPIOB CLK_GPIO(1)
#define CLK_GPIOC CLK_GPIO(2)
#define CLK_GPIOD CLK_GPIO(3)
#define CLK_GPIOE CLK_GPIO(4)
#define CLK_GPIOF CLK_GPIO(5)
#define CLK_GPIOG CLK_GPIO(6)
#define CLK_GPIOH CLK_GPIO(7)
#define CLK_GPIOI CLK_GPIO(8)
#define CLK_GPIOJ CLK_GPIO(9)
#define CLK_GPIOK CLK_GPIO(10)

#define CLK_CRC   CLK_ID(STM32H7_CLK_AHB4, 19)

#define CLK_USART2 CLK_ID(STM32H7_CLK_APB1L, 17)
#define CLK_USART3 CLK_ID(STM32H7_CLK_APB1L, 18)
#define CLK_USART4 CLK_ID(STM32H7_CLK_APB1L, 19)
#define CLK_USART5 CLK_ID(STM32H7_CLK_APB1L, 20)

#define CLK_I2C1 CLK_ID(STM32H7_CLK_APB1L, 21)
#define CLK_I2C2 CLK_ID(STM32H7_CLK_APB1L, 22)
#define CLK_I2C3 CLK_ID(STM32H7_CLK_APB1L, 23)
#define CLK_I2C4 CLK_ID(STM32H7_CLK_APB4, 7)

#define CLK_LPTIM1 CLK_ID(STM32H7_CLK_APB1L, 9)

#define CLK_SAI1 CLK_ID(STM32H7_CLK_APB2, 22)
#define CLK_SAI2 CLK_ID(STM32H7_CLK_APB2, 23)
#define CLK_SAI3 CLK_ID(STM32H7_CLK_APB2, 24)

#define CLK_DMA1 CLK_ID(STM32H7_CLK_AHB1, 0)
#define CLK_DMA2 CLK_ID(STM32H7_CLK_AHB1, 1)


static inline void
clk_enable(uint16_t id)
{
  reg_set_bit(STM32H7_RCC_BASE + (id >> 8), id & 0xff);
}

static inline void
clk_disable(uint16_t id)
{
  reg_clr_bit(STM32H7_RCC_BASE + (id >> 8), id & 0xff);
}

unsigned int clk_get_freq(uint16_t id);

void stm32h7_init_pll(void);

