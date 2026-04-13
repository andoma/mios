#pragma once

#include "stm32n6_reg.h"

#define RCC_BASE 0x56028000

#define RCC_CR            (RCC_BASE + 0x000)
#define RCC_SR            (RCC_BASE + 0x004)
#define RCC_CFGR1         (RCC_BASE + 0x020)
#define RCC_CFGR2         (RCC_BASE + 0x024)

#define RCC_PLLxCFGR1(x)  (RCC_BASE + 0x70 + (x) * 0x10)
#define RCC_PLLxCFGR2(x)  (RCC_BASE + 0x74 + (x) * 0x10)
#define RCC_PLLxCFGR3(x)  (RCC_BASE + 0x78 + (x) * 0x10)

#define RCC_ICxCFGR(x)    (RCC_BASE + 0xc0 + (x) * 4)

#define RCC_CCIPR2        (RCC_BASE + 0x148)
#define RCC_CCIPR6        (RCC_BASE + 0x158)

#define RCC_DIVENR        (RCC_BASE + 0x240)
#define RCC_BUSENR        (RCC_BASE + 0x244)
#define RCC_MEMENR        (RCC_BASE + 0x24C)

#define RCC_AHB4ENR       (RCC_BASE + 0x25c)
#define RCC_AHB5ENR       (RCC_BASE + 0x260)
#define RCC_APB1LENR      (RCC_BASE + 0x264)

#define RCC_APB2ENR       (RCC_BASE + 0x26c)
#define RCC_APB4ENR1      (RCC_BASE + 0x274)
#define RCC_APB4ENR2      (RCC_BASE + 0x278)


#define CLK_BSEC      CLK_ID(RCC_APB4ENR2, 1)

#define CLK_GPIO(x)   CLK_ID(RCC_AHB4ENR, (x))

#define CLK_USART1    CLK_ID(RCC_APB2ENR, 4)
#define CLK_USART2    CLK_ID(RCC_APB1LENR, 17)
#define CLK_USART3    CLK_ID(RCC_APB1LENR, 18)
#define CLK_USART4    CLK_ID(RCC_APB1LENR, 19)
#define CLK_USART5    CLK_ID(RCC_APB1LENR, 20)
#define CLK_USART6    CLK_ID(RCC_APB2ENR, 5)
#define CLK_USART7    CLK_ID(RCC_APB1LENR, 30)
#define CLK_USART8    CLK_ID(RCC_APB1LENR, 31)
#define CLK_USART9    CLK_ID(RCC_APB2ENR, 6)
#define CLK_USART10   CLK_ID(RCC_APB2ENR, 7)

#define CLK_I2C1      CLK_ID(RCC_APB1LENR, 21)
#define CLK_I2C2      CLK_ID(RCC_APB1LENR, 22)
#define CLK_I2C3      CLK_ID(RCC_APB1LENR, 23)
#define CLK_I2C4      CLK_ID(RCC_APB4ENR1, 7)

#define CLK_SPI1      CLK_ID(RCC_APB2ENR, 12)
#define CLK_SPI2      CLK_ID(RCC_APB1LENR, 14)
#define CLK_SPI3      CLK_ID(RCC_APB1LENR, 15)
#define CLK_SPI4      CLK_ID(RCC_APB2ENR, 13)
#define CLK_SPI5      CLK_ID(RCC_APB2ENR, 20)
#define CLK_SPI6      CLK_ID(RCC_APB4ENR1, 5)

#define CLK_TIM6      CLK_ID(RCC_APB1LENR, 4)
#define CLK_TIM7      CLK_ID(RCC_APB1LENR, 5)

#define CLK_XSPI1     CLK_ID(RCC_AHB5ENR, 5)
#define CLK_XSPI2     CLK_ID(RCC_AHB5ENR, 12)
#define CLK_XSPIM     CLK_ID(RCC_AHB5ENR, 13)
#define CLK_XSPI3     CLK_ID(RCC_AHB5ENR, 17)

#define CLK_ETH1MACEN  CLK_ID(RCC_AHB5ENR, 22)
#define CLK_ETH1TXEN   CLK_ID(RCC_AHB5ENR, 23)
#define CLK_ETH1RXEN   CLK_ID(RCC_AHB5ENR, 24)
#define CLK_ETH1EN     CLK_ID(RCC_AHB5ENR, 25)

#define CLK_OTG1PHYCTL CLK_ID(RCC_AHB5ENR, 22)
#define CLK_OTG2PHYCTL CLK_ID(RCC_AHB5ENR, 24)
#define CLK_OTG1       CLK_ID(RCC_AHB5ENR, 26)
#define CLK_OTG1PHY    CLK_ID(RCC_AHB5ENR, 27)
#define CLK_OTG2PHY    CLK_ID(RCC_AHB5ENR, 28)
#define CLK_OTG2       CLK_ID(RCC_AHB5ENR, 29)

#define CLK_ID(reg, bit) ((((reg) & 0x7ff) << 5) | (bit))

__attribute__((always_inline))
static inline void
clk_enable(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 5), id & 0x1f);
  asm volatile("dsb");
}

__attribute__((always_inline))
static inline void
clk_disable(uint16_t id)
{
  reg_clr_bit(RCC_BASE + (id >> 5), id & 0x1f);
  asm volatile("dsb");
}

__attribute__((always_inline))
static inline int
clk_is_enabled(uint16_t id)
{
  return reg_get_bit(RCC_BASE + (id >> 5), id & 0x1f);
}

__attribute__((always_inline))
static inline void
rst_assert(uint16_t id)
{
  uint16_t reg = id >> 5;
  reg -= 0x40;
  reg_set_bit(RCC_BASE + reg, id & 0x1f);
  asm volatile("dsb");
}

__attribute__((always_inline))
static inline void
rst_deassert(uint16_t id)
{
  uint16_t reg = id >> 5;
  reg -= 0x40;
  reg_clr_bit(RCC_BASE + reg, id & 0x1f);
  asm volatile("dsb");
}

unsigned int clk_get_freq(uint16_t id);

extern uint32_t stm32n6_hse_freq;

const char *stm32n6_init_pll(unsigned int hse_freq);
