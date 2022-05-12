#pragma once

#include "stm32wb_reg.h"


#define RCC_BASE 0x58000000

#define RCC_CR      (RCC_BASE + 0x00)
#define RCC_CFGR    (RCC_BASE + 0x08)
#define RCC_PLLCFGR (RCC_BASE + 0x0c)

#define RCC_AHB1ENR  (RCC_BASE + 0x48)
#define RCC_AHB2ENR  (RCC_BASE + 0x4c)
#define RCC_APB1ENR2 (RCC_BASE + 0x5c)
#define RCC_APB2ENR  (RCC_BASE + 0x60)
#define RCC_CCIPR    (RCC_BASE + 0x88)

#define CLK_ID(reg, bit) (((reg & 0xff) << 8) | (bit))

#define CLK_GPIO(x) CLK_ID(RCC_AHB2ENR, (x))

#define CLK_GPIOA CLK_GPIO(0)
#define CLK_GPIOB CLK_GPIO(1)
#define CLK_GPIOC CLK_GPIO(2)
#define CLK_GPIOE CLK_GPIO(4)
#define CLK_GPIOH CLK_GPIO(7)

#define CLK_ADC     CLK_ID(RCC_AHB2ENR, 13)

#define CLK_DMA1    CLK_ID(RCC_AHB1ENR, 0)
#define CLK_DMA2    CLK_ID(RCC_AHB1ENR, 1)
#define CLK_DMAMUX1 CLK_ID(RCC_AHB1ENR, 2)
#define CLK_CRC     CLK_ID(RCC_AHB1ENR, 12)

#define CLK_I2C1    CLK_ID(RCC_APB1ENR1, 21)
#define CLK_I2C3    CLK_ID(RCC_APB1ENR1, 23)

#define CLK_USART1  CLK_ID(RCC_APB2ENR,  14)
#define CLK_LPUART1 CLK_ID(RCC_APB1ENR2, 14)

#define CLK_SPI1    CLK_ID(RCC_APB2ENR,  12)

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

void stm32wb_use_hse(void);
