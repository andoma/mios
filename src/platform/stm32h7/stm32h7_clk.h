#pragma once

#include "stm32h7_reg.h"

#define RCC_BASE       0x58024400

#define RCC_CR            (RCC_BASE + 0x00)
#define RCC_CFGR          (RCC_BASE + 0x10)

#define RCC_D1CFGR        (RCC_BASE + 0x18)
#define RCC_D2CFGR        (RCC_BASE + 0x1c)
#define RCC_D3CFGR        (RCC_BASE + 0x20)
#define RCC_PLLCKSELR     (RCC_BASE + 0x28)
#define RCC_PLLCFGR       (RCC_BASE + 0x2c)
#define RCC_PLL1DIVR      (RCC_BASE + 0x30)
#define RCC_PLL2DIVR      (RCC_BASE + 0x38)
#define RCC_PLL3DIVR      (RCC_BASE + 0x40)
#define RCC_D2CCIP1R      (RCC_BASE + 0x50)
#define RCC_D2CCIP2R      (RCC_BASE + 0x54)

#define RCC_AHB1RSTR      (RCC_BASE + 0x80)
#define RCC_APB1LRSTR     (RCC_BASE + 0x90)
#define RCC_APB1HRSTR     (RCC_BASE + 0x94)

#define RCC_AHB1ENR       (RCC_BASE + 0xd8)
#define RCC_AHB4ENR       (RCC_BASE + 0xe0)
#define RCC_APB1LENR      (RCC_BASE + 0xe8)
#define RCC_APB1HENR      (RCC_BASE + 0xec)
#define RCC_APB2ENR       (RCC_BASE + 0xf0)
#define RCC_APB4ENR       (RCC_BASE + 0xf4)

#define RCC_AHB1LPENR     (RCC_BASE + 0x100)

#define CLK_ID(reg, bit) ((((reg) & 0xff) << 8) | (bit))


#define CLK_FDCAN     CLK_ID(RCC_APB1HENR, 8)
#define CLK_CSR       CLK_ID(RCC_APB1HENR, 1)

#define CLK_DMA1      CLK_ID(RCC_AHB1ENR, 0)
#define CLK_DMA2      CLK_ID(RCC_AHB1ENR, 1)

#define CLK_ETH1MACEN CLK_ID(RCC_AHB1ENR, 15)
#define CLK_ETH1RXEN  CLK_ID(RCC_AHB1ENR, 16)
#define CLK_ETH1TXEN  CLK_ID(RCC_AHB1ENR, 17)
#define CLK_OTG       CLK_ID(RCC_AHB1ENR, 25)

#define CLK_GPIO(x)   CLK_ID(RCC_AHB4ENR, (x))

#define CLK_GPIOA     CLK_GPIO(0)
#define CLK_GPIOB     CLK_GPIO(1)
#define CLK_GPIOC     CLK_GPIO(2)
#define CLK_GPIOD     CLK_GPIO(3)
#define CLK_GPIOE     CLK_GPIO(4)
#define CLK_GPIOF     CLK_GPIO(5)
#define CLK_GPIOG     CLK_GPIO(6)
#define CLK_GPIOH     CLK_GPIO(7)
#define CLK_GPIOI     CLK_GPIO(8)
#define CLK_GPIOJ     CLK_GPIO(9)
#define CLK_GPIOK     CLK_GPIO(10)

#define CLK_CRC       CLK_ID(RCC_AHB4ENR, 19)

#define CLK_USART2    CLK_ID(RCC_APB1LENR, 17)
#define CLK_USART3    CLK_ID(RCC_APB1LENR, 18)
#define CLK_USART4    CLK_ID(RCC_APB1LENR, 19)
#define CLK_USART5    CLK_ID(RCC_APB1LENR, 20)

#define CLK_I2C1      CLK_ID(RCC_APB1LENR, 21)
#define CLK_I2C2      CLK_ID(RCC_APB1LENR, 22)
#define CLK_I2C3      CLK_ID(RCC_APB1LENR, 23)

#define CLK_SYSCFG    CLK_ID(RCC_APB4ENR, 1)
#define CLK_I2C4      CLK_ID(RCC_APB4ENR, 7)

#define CLK_LPTIM1    CLK_ID(RCC_APB1LENR, 9)

#define CLK_SAI1      CLK_ID(RCC_APB2ENR, 22)
#define CLK_SAI2      CLK_ID(RCC_APB2ENR, 23)
#define CLK_SAI3      CLK_ID(RCC_APB2ENR, 24)



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

unsigned int clk_get_freq(uint16_t id);

#define STM32H7_HSE_NO_XTAL   0x1 // HSE is a clock, no xtal

const char *stm32h7_init_pll(unsigned int hse_freq, uint32_t flags);

#define RST_ID(reg, bit) ((((reg) & 0xff) << 8) | (bit))

#define RST_OTG       RST_ID(RCC_AHB1RSTR, 25)
#define RST_CSR       RST_ID(RCC_APB1HRSTR, 1)

void reset_peripheral(uint16_t id);

