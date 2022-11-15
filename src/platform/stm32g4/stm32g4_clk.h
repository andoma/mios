#pragma once

#include "stm32g4_reg.h"

#define RCC_BASE 0x40021000

#define RCC_AHB1ENR  0x48
#define RCC_AHB2ENR  0x4c
#define RCC_APB1ENR1 0x58
#define RCC_APB1ENR2 0x5c
#define RCC_APB2ENR  0x60

#define CLK_ID(reg, bit) (((reg) << 8) | (bit))

#define CLK_GPIO(x) CLK_ID(RCC_AHB2ENR, (x))

#define CLK_GPIOA CLK_GPIO(0)
#define CLK_GPIOB CLK_GPIO(1)
#define CLK_GPIOC CLK_GPIO(2)
#define CLK_GPIOD CLK_GPIO(3)
#define CLK_GPIOE CLK_GPIO(4)
#define CLK_GPIOF CLK_GPIO(5)
#define CLK_GPIOG CLK_GPIO(6)

#define CLK_USART1 CLK_ID(RCC_APB2ENR, 14)
#define CLK_USART2 CLK_ID(RCC_APB1ENR1, 17)
#define CLK_USART3 CLK_ID(RCC_APB1ENR1, 18)


#define CLK_SPI1  CLK_ID(RCC_APB2ENR,  12)
#define CLK_SPI2  CLK_ID(RCC_APB1ENR1, 14)
#define CLK_SPI3  CLK_ID(RCC_APB1ENR1, 15)

#define CLK_I2C1  CLK_ID(RCC_APB1ENR1, 21)
#define CLK_I2C2  CLK_ID(RCC_APB1ENR1, 22)
#define CLK_I2C3  CLK_ID(RCC_APB1ENR1, 30)
#define CLK_I2C4  CLK_ID(RCC_APB1ENR2, 1)

#define CLK_DMA1    CLK_ID(RCC_AHB1ENR, 0)
#define CLK_DMA2    CLK_ID(RCC_AHB1ENR, 1)
#define CLK_DMAMUX1 CLK_ID(RCC_AHB1ENR, 2)


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

void stm32g4_init_pll(int hse_freq);

