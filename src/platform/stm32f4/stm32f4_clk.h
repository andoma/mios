#pragma once

#define CLK_AHB1 0x30
#define CLK_APB1 0x40
#define CLK_APB2 0x44

#define RCC_BASE 0x40023800

#define CLK_ID(reg, bit) (((reg) << 8) | (bit))

#define CLK_SPI1 CLK_ID(CLK_APB2, 12)
#define CLK_SPI2 CLK_ID(CLK_APB1, 14)
#define CLK_SPI3 CLK_ID(CLK_APB1, 15)

static inline void
clk_enable(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 8), id & 0xff);
}
