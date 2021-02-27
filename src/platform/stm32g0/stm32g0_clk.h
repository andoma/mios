#pragma once

#include "stm32g0.h"


#define RCC_BASE 0x40021000

#define RCC_APBRSTR1 0x2c

#define RCC_IOPENR  0x34
#define RCC_AHBENR  0x38
#define RCC_APBENR1 0x3c
#define RCC_APBENR2 0x40

#define CLK_ID(reg, bit) (((reg) << 8) | (bit))

#define CLK_CRC CLK_ID(RCC_AHBENR, 12)

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

#define CLK_I2C1 CLK_ID(RCC_APBENR1, 21)
#define CLK_I2C2 CLK_ID(RCC_APBENR1, 22)
#define CLK_I2C3 CLK_ID(RCC_APBENR1, 23)

#define RST_I2C1 CLK_ID(RCC_APBRSTR1, 21)
#define RST_I2C2 CLK_ID(RCC_APBRSTR1, 22)
#define RST_I2C3 CLK_ID(RCC_APBRSTR1, 23)

#define CLK_TIM7 CLK_ID(RCC_APBENR1,  5)


void reset_peripheral(uint16_t id);

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
