#include <mios/mios.h>

#include <stddef.h>

#include "stm32h7_clk.h"
#include "stm32h7_pwr.h"

#define SCB_CCR   0xe000ed14

#define FLASH_ACR 0x52002000


#define VOS_LEVEL_0 0
#define VOS_LEVEL_1 3
#define VOS_LEVEL_2 2
#define VOS_LEVEL_3 1


static void
set_flash_latency(int axi_freq, int vos)
{
  // TODO: Compute these based on AXI frequency

  int latency;
  int wr_high_freq;

  latency = 4;
  wr_high_freq = 3;

  uint32_t val = latency | (wr_high_freq << 4);
  reg_wr(FLASH_ACR, val);
  while(reg_rd(FLASH_ACR) != val) {}
}


static void
voltage_scaling(int level)
{
  while(!(reg_rd(PWR_CSR1) & (1 << 13))) {}

  // Set VOS
  reg_set_bits(PWR_D3CR, 14, 2, level);

  while(!(reg_rd(PWR_D3CR) & (1 << 13))) {}
}



/*

 D1 DOMAIN
   CPU Freq  @ CPU_SYSCLK_MHZ
   |
   +- /HPRE \  CPU_SYSCLK_MHZ / 2
            |
            +------------ => AXI
            +------------ => AHB3  CPU_SYSCLK_MHZ / 4
            +--- /D1PPRE  => APB3  CPU_SYSCLK_MHZ / 4
            |
 D2 DOMAIN  |
            |
            +------------ => AHB1
            +------------ => AHB2
            +--- /D2PPRE1 => APB1  CPU_SYSCLK_MHZ / 4
            +--- /D2PPRE2 => APB2  CPU_SYSCLK_MHZ / 4
            |
 D3 DOMAIN  |
            +------------ => AHB4
            +--- /D3PPRE  => APB4  CPU_SYSCLK_MHZ / 4
*/


const char *
stm32h7_init_pll(unsigned int hse_freq, uint32_t flags)
{
  reg_clr_bit(PWR_CR3, 2); // Disable SMPS
  while(reg_get_bit(PWR_CSR1, 13) == 0) {}

  int sysclk_freq = CPU_SYSCLK_MHZ;
  int axi_freq = sysclk_freq / 2;

  const uint32_t pll1m = 1; // Input prescaler (1 == divide  by 1)
  const uint32_t pll2m = 1; // Input prescaler (1 == divide  by 1)
  const uint32_t pll3m = 1; // Input prescaler (1 == divide  by 1)

  int vos;
  if(sysclk_freq > 400) {
    vos = VOS_LEVEL_0;
  } else if(sysclk_freq > 300) {
    vos = VOS_LEVEL_1;
  } else if(sysclk_freq > 170) {
    vos = VOS_LEVEL_2;
  } else {
    vos = VOS_LEVEL_3;
  }

  int pllscr;

  if(hse_freq) {

    if(flags & STM32H7_HSE_NO_XTAL) {
      // Use external 8MHz clock
      // HSE osc bypass (no crystal)
      reg_set_bit(RCC_CR, 18);
    }

    // HSE clock enable
    reg_set_bit(RCC_CR, 16);

    // Wait for stable HSE clock
    while(!(reg_rd(RCC_CR) & (1 << 17))) {}

    pllscr = 2; // HSE is PLL clock
  } else {
    // HSI Clock divide by 8   64 -> 8 MHz
    reg_set_bits(RCC_CR, 3, 2, 3);

    pllscr = 0; // HSI is PLL clock
    hse_freq = 8;
  }

  uint32_t plln = (sysclk_freq / hse_freq) - 1;
  uint32_t pllp = 0;
  uint32_t pllq = 20;

  uint32_t d1cpre  = 0;
  uint32_t hpre    = 0x8; // Divide by 2
  uint32_t d1ppre  = 0x4; // Divide by 2

  uint32_t d2ppre1 = 0x4; // Divide by 2
  uint32_t d2ppre2 = 0x4; // Divide by 2

  uint32_t d3ppre  = 0x4; // Divide by 2

  // Configure clock dividers for domains
  reg_wr(RCC_D1CFGR,
         (hpre << 0) |
         (d1ppre << 4) |
         (d1cpre << 8));

  reg_wr(RCC_D2CFGR,
         (d2ppre1 << 4) |
         (d2ppre2 << 8));

  reg_wr(RCC_D3CFGR, d3ppre << 4);


  // Set prescalers for PLLx
  reg_wr(RCC_PLLCKSELR,
         pllscr |
         pll1m << 4 |
         pll2m << 12 |
         pll3m << 20);


  // PLLx input rage (8-16 MHz) - output enables
  reg_wr(RCC_PLLCFGR,
         (3 << 2)  | // PLL1 Input range
         (3 << 6)  | // PLL2 Input range
         (3 << 10) | // PLL3 Input range
         (1 << 16) | // PLL1(p) enable
         (1 << 17) | // PLL1(q) enable
         0);

  // Write multiplier and output divider
  reg_wr(RCC_PLL1DIVR, (pllq << 16) | (pllp << 9) | plln);

  set_flash_latency(axi_freq, vos);

  voltage_scaling(vos);

  // Start PLL1
  reg_set_bit(RCC_CR, 24);

  // Wait for PLL1
  while(!(reg_rd(RCC_CR) & (1 << 25))) {}

  // Switch to PLL1
  reg_set_bits(RCC_CFGR, 0, 3, 3);
  while(((reg_rd(RCC_CFGR) >> 3) & 0x7) != 3) {}

  return NULL;
}


unsigned int
clk_get_freq(uint16_t id)
{
  // TODO: Fix this incorrect hack
  return CPU_SYSTICK_RVR / 4;
}

#include <stdio.h>
void
reset_peripheral(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 8), id & 0xff);
  reg_clr_bit(RCC_BASE + (id >> 8), id & 0xff);
}
