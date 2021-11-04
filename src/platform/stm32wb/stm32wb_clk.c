#include "stm32wb_clk.h"

#define FLASH_BASE 0x58004000

#define FLASH_ACR  (FLASH_BASE + 0x00)


int
clk_get_freq(uint16_t id)
{
  return CPU_SYSTICK_RVR;
}


void
stm32wb_use_hse(void)
{
  reg_wr(FLASH_ACR, 0x703); // D-CACHE I-CACHE PREFETCH, 3 wait states

  reg_set_bit(RCC_CR, 16); // HSEON
  while(!(reg_rd(RCC_CR) & (1 << 17))) {} // Wait for external oscillator

  uint32_t pllcfgr = 0;

  pllcfgr |= 3; // PLL input is HSE

  pllcfgr |= (7 << 4); // Divide HSE by 8

  pllcfgr |= (32 << 8); // N now at 128MHz

  pllcfgr |= (1 << 29); // R = 64MHz
  pllcfgr |= (1 << 28); // PLLREN

  reg_wr(RCC_PLLCFGR, pllcfgr);

  reg_set_bit(RCC_CR, 24);
  while(!(reg_rd(RCC_CR) & (1 << 25))) {} // Wait for pll

  reg_set_bits(RCC_CFGR, 0, 2, 3); // Use PLL as system clock
  while((reg_rd(RCC_CFGR) & 0xc) != 0xc) {}
}
