#pragma once

#include "stm32g4_reg.h"

#define RCC_BASE    0x40021000

#define RCC_CR        (RCC_BASE + 0x00)
#define RCC_ICSCR     (RCC_BASE + 0x04)
#define RCC_CFGR      (RCC_BASE + 0x08)
#define RCC_PLLCFGR   (RCC_BASE + 0x0c)
#define RCC_CCIPR     (RCC_BASE + 0x88)
#define RCC_CRRCR     (RCC_BASE + 0x98)

#define RCC_AHB1RSTR  (RCC_BASE + 0x28)
#define RCC_AHB2RSTR  (RCC_BASE + 0x2c)
#define RCC_AHB3RSTR  (RCC_BASE + 0x30)
#define RCC_APB1RSTR1 (RCC_BASE + 0x38)
#define RCC_APB1RSTR2 (RCC_BASE + 0x3c)
#define RCC_APB2RSTR  (RCC_BASE + 0x40)

#define RCC_AHB1ENR   (RCC_BASE + 0x48)
#define RCC_AHB2ENR   (RCC_BASE + 0x4c)
#define RCC_AHB3ENR   (RCC_BASE + 0x50)
#define RCC_APB1ENR1  (RCC_BASE + 0x58)
#define RCC_APB1ENR2  (RCC_BASE + 0x5c)
#define RCC_APB2ENR   (RCC_BASE + 0x60)

#define RCC_BDCR      (RCC_BASE + 0x90)
#define RCC_CSR       (RCC_BASE + 0x94)

#define CLK_ID(reg, bit) ((((reg) & 0xff) << 8) | (bit))

#define CLK_GPIO(x) CLK_ID(RCC_AHB2ENR, (x))

#define CLK_GPIOA CLK_GPIO(0)
#define CLK_GPIOB CLK_GPIO(1)
#define CLK_GPIOC CLK_GPIO(2)
#define CLK_GPIOD CLK_GPIO(3)
#define CLK_GPIOE CLK_GPIO(4)
#define CLK_GPIOF CLK_GPIO(5)
#define CLK_GPIOG CLK_GPIO(6)

#define CLK_ADC12   CLK_ID(RCC_AHB2ENR, 13)
#define CLK_ADC345  CLK_ID(RCC_AHB2ENR, 14)

#define CLK_DAC(x)  CLK_ID(RCC_AHB2ENR, (x) + 15)

#define CLK_SYSCFG CLK_ID(RCC_APB2ENR, 0)

#define CLK_USART1 CLK_ID(RCC_APB2ENR, 14)
#define CLK_USART2 CLK_ID(RCC_APB1ENR1, 17)
#define CLK_USART3 CLK_ID(RCC_APB1ENR1, 18)

#define CLK_TIM1   CLK_ID(RCC_APB2ENR, 11)
#define CLK_TIM2   CLK_ID(RCC_APB1ENR1, 0)
#define CLK_TIM3   CLK_ID(RCC_APB1ENR1, 1)
#define CLK_TIM4   CLK_ID(RCC_APB1ENR1, 2)
#define CLK_TIM5   CLK_ID(RCC_APB1ENR1, 3)
#define CLK_TIM6   CLK_ID(RCC_APB1ENR1, 4)
#define CLK_TIM7   CLK_ID(RCC_APB1ENR1, 5)
#define CLK_TIM8   CLK_ID(RCC_APB2ENR, 13)
#define CLK_TIM15  CLK_ID(RCC_APB2ENR, 16)
#define CLK_TIM16  CLK_ID(RCC_APB2ENR, 17)
#define CLK_TIM17  CLK_ID(RCC_APB2ENR, 18)
#define CLK_TIM20  CLK_ID(RCC_APB2ENR, 20)

#define CLK_HRTIM1 CLK_ID(RCC_APB2ENR, 26)

#define CLK_SPI1  CLK_ID(RCC_APB2ENR,  12)
#define CLK_SPI2  CLK_ID(RCC_APB1ENR1, 14)
#define CLK_SPI3  CLK_ID(RCC_APB1ENR1, 15)

#define CLK_I2C1  CLK_ID(RCC_APB1ENR1, 21)
#define CLK_I2C2  CLK_ID(RCC_APB1ENR1, 22)
#define CLK_I2C3  CLK_ID(RCC_APB1ENR1, 30)
#define CLK_I2C4  CLK_ID(RCC_APB1ENR2, 1)

#define CLK_USB   CLK_ID(RCC_APB1ENR1, 23)

#define CLK_FDCAN CLK_ID(RCC_APB1ENR1, 25)

#define CLK_DMA1    CLK_ID(RCC_AHB1ENR, 0)
#define CLK_DMA2    CLK_ID(RCC_AHB1ENR, 1)
#define CLK_DMAMUX1 CLK_ID(RCC_AHB1ENR, 2)

#define CLK_CRC     CLK_ID(RCC_AHB1ENR, 12)

#define CLK_SAI1    CLK_ID(RCC_APB2ENR, 21)

#define CLK_PWR CLK_ID(RCC_APB1ENR1, 28)
#define CLK_RTC CLK_ID(RCC_APB1ENR1, 10)


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

void stm32g4_init_pll(uint8_t hse_freq, uint8_t p_freq);

void stm32g4_reinit_pll(void);

void stm32g4_deinit_pll(void);

void clk_enable_hsi48(void);

void stm32g4_deinit_clk(void);

void stm32g4_lse_enable(void);

void stm32g4_enable_backup_domain(void);
