#pragma once

#include "stm32f4_reg.h"


#define RST_AHB1 0x10
#define RST_AHB2 0x14
#define RST_AHB3 0x18
#define RST_APB1 0x20
#define RST_APB2 0x24

#define CLK_AHB1 0x30
#define CLK_AHB2 0x34
#define CLK_APB1 0x40
#define CLK_APB2 0x44

#define RCC_BASE    0x40023800

#define RCC_CR      (RCC_BASE + 0x00)
#define RCC_PLLCFGR (RCC_BASE + 0x04)
#define RCC_CFGR    (RCC_BASE + 0x08)
#define RCC_PLLCFGR (RCC_BASE + 0x04)
#define RCC_PLLI2S  (RCC_BASE + 0x84)

#define RCC_APB2ENR (RCC_BASE + CLK_APB2)
#define RCC_AHB1ENR (RCC_BASE + CLK_AHB1)
#define RCC_APB1ENR (RCC_BASE + CLK_APB1)

#define CLK_ID(reg, bit) (((reg & 0xff) << 8) | (bit))

#define CLK_SPI1 CLK_ID(CLK_APB2, 12)
#define CLK_SPI2 CLK_ID(CLK_APB1, 14)
#define CLK_SPI3 CLK_ID(CLK_APB1, 15)

#define CLK_USART1 CLK_ID(CLK_APB2, 4)
#define CLK_USART2 CLK_ID(CLK_APB1, 17)
#define CLK_USART3 CLK_ID(CLK_APB1, 18)
#define CLK_UART4  CLK_ID(CLK_APB1, 19)
#define CLK_UART5  CLK_ID(CLK_APB1, 20)
#define CLK_USART6 CLK_ID(CLK_APB2, 5)

#define CLK_ADC1  CLK_ID(CLK_APB2, 8)
#define CLK_ADC2  CLK_ID(CLK_APB2, 9)
#define CLK_ADC3  CLK_ID(CLK_APB2, 10)

#define CLK_ADCx(x) CLK_ID(CLK_APB2, 8 + (x))

#define CLK_TIM1  CLK_ID(CLK_APB2, 0)
#define CLK_TIM2  CLK_ID(CLK_APB1, 0)
#define CLK_TIM3  CLK_ID(CLK_APB1, 1)
#define CLK_TIM4  CLK_ID(CLK_APB1, 2)
#define CLK_TIM5  CLK_ID(CLK_APB1, 3)
#define CLK_TIM6  CLK_ID(CLK_APB1, 4)
#define CLK_TIM7  CLK_ID(CLK_APB1, 5)
#define CLK_TIM8  CLK_ID(CLK_APB2, 1)
#define CLK_TIM9  CLK_ID(CLK_APB2, 16)
#define CLK_TIM10 CLK_ID(CLK_APB2, 17)
#define CLK_TIM11 CLK_ID(CLK_APB2, 18)
#define CLK_TIM12 CLK_ID(CLK_APB1, 6)
#define CLK_TIM13 CLK_ID(CLK_APB1, 7)
#define CLK_TIM14 CLK_ID(CLK_APB1, 8)

#define CLK_PWR   CLK_ID(CLK_APB1, 28)

#define CLK_I2C1  CLK_ID(CLK_APB1, 21)
#define CLK_I2C2  CLK_ID(CLK_APB1, 22)
#define CLK_I2C3  CLK_ID(CLK_APB1, 23)
#define CLK_I2C(x) CLK_ID(CLK_APB1, 21 + (x))

#define CLK_CAN(x) CLK_ID(CLK_APB1, 24 + (x))

#define CLK_DAC    CLK_ID(CLK_APB1, 29)

#define RST_I2C(x) CLK_ID(RST_APB1, 21 + (x))

#define CLK_ETHRX CLK_ID(CLK_AHB1, 27)
#define CLK_ETHTX CLK_ID(CLK_AHB1, 26)
#define CLK_ETH   CLK_ID(CLK_AHB1, 25)
#define RST_ETH   CLK_ID(RST_AHB1, 25)

#define CLK_DMA1  CLK_ID(CLK_AHB1, 21)
#define CLK_DMA2  CLK_ID(CLK_AHB1, 22)
#define CLK_DMA(x)  CLK_ID(CLK_AHB1, 21 + (x))

#define CLK_GPIO(x) CLK_ID(CLK_AHB1, x)

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


#define CLK_SYSCFG  CLK_ID(CLK_APB2, 14)

#define CLK_CCMDATARAMEN CLK_ID(CLK_AHB1, 20)
#define CLK_RNG          CLK_ID(CLK_AHB2, 6)

#define CLK_OTG        CLK_ID(CLK_AHB2, 7)
#define RST_OTG        CLK_ID(RST_AHB2, 7)


void reset_peripheral(uint16_t id);

static inline void
clk_enable(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 8), id & 0xff);
}

static inline int
clk_is_enabled(uint16_t id)
{
  return reg_get_bit(RCC_BASE + (id >> 8), id & 0xff);
}

static inline void
clk_disable(uint16_t id)
{
  reg_clr_bit(RCC_BASE + (id >> 8), id & 0xff);
}

int clk_get_freq(uint16_t id);

void stm32f4_init_pll(int hse_freq);

void stm32f4_clk_deinit(void);
