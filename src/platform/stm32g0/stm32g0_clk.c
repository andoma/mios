#include "stm32g0_reg.h"
#include "stm32g0_clk.h"

void
reset_peripheral(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 8), id & 0xff);
  reg_clr_bit(RCC_BASE + (id >> 8), id & 0xff);
}


#define FLASH_BASE 0x40022000
#define FLASH_ACR (FLASH_BASE + 0x00)


void
stm32g0_init_pll(void)
{
  if(CPU_SYSCLK_MHZ == 16)
    return;

  // I-CACHE PREFETCH, 2 wait states for 64MHz
  reg_wr(FLASH_ACR, (reg_rd(FLASH_ACR) & ~3) | 0x302);

  uint32_t pllcfgr = 0;

  uint32_t pllm = 3; // divide by 4 -> (HSI16 is now 4MHZ)
  uint32_t plln = CPU_SYSCLK_MHZ;
  uint32_t pllr = 3; // divide by 4 again

  pllcfgr |= 0b10; // PLL input is HSI16

  pllcfgr |= pllr << 29;
  pllcfgr |= (1 << 28); // PLLREN

  pllcfgr |= plln << 8;
  pllcfgr |= pllm << 4;

  reg_wr(RCC_PLLCFGR, pllcfgr);

  reg_set_bit(RCC_CR, 24);
  while(!(reg_rd(RCC_CR) & (1 << 25))) {} // Wait for pll

  reg_set_bits(RCC_CFGR, 0, 3, 2); // Use PLL as system clock

  while(((reg_rd(RCC_CFGR) >> 3) & 0x7) != 0x2) {}
}


void
stm32g0_stop_pll(void)
{
  reg_set_bits(RCC_CFGR, 0, 3, 0); // Use HSI16 as system clock
  while(((reg_rd(RCC_CFGR) >> 3) & 0x7) != 0x0) {}
  reg_clr_bit(RCC_CR, 24);
}
