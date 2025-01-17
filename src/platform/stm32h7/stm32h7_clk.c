#include <mios/mios.h>

#include "stm32h7_clk.h"
#include "stm32h7_pwr.h"


#define FLASH_ACR 0x52002000


#define VOS_LEVEL_0 0 // 480 MHz
#define VOS_LEVEL_1 3 // 400 MHz
#define VOS_LEVEL_2 2 // 300 MHz
#define VOS_LEVEL_3 1 // 200 MHz


static void
set_flash_latency(int axi_freq, int vos)
{
  // Table 17. FLASH recommended number of wait states and programming delay

  int latency;
  int wr_high_freq;
  if(axi_freq == 200 && vos == VOS_LEVEL_1) {
    latency = 2;
    wr_high_freq = 2;
  } else {
    panic("Unsupported flash latency axi=%d vos=%d", axi_freq, vos);
  }

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
   CPU Freq   480Mhz
   |
   +- /HPRE \  Max 240MHz (Divide by 2)
            |
            +------------ => AXI
            +------------ => AHB3
            +--- /D1PPRE  => APB3
            |
 D2 DOMAIN  |
            |
            +------------ => AHB1
            +------------ => AHB2
            +--- /D2PPRE1 => APB1
            +--- /D2PPRE2 => APB2
            |
 D3 DOMAIN  |
            +------------ => AHB4
            +--- /D3PPRE  => APB4


*/




static void
wait_voltages_ready(void)
{
  while(reg_get_bit(PWR_CSR1, 13) == 0) {}
}



void
stm32h7_init_pll(void)
{
  //reg_wr(PWR_CR3, (reg_rd(PWR_CR3) | (1 << 1)) & ~((1 << 0) | (1 << 2)));

           // Lock LDO setup
  reg_clr_bit(PWR_CR3, 2);
  wait_voltages_ready();

  int sysclk_freq = 400;
  int axi_freq = sysclk_freq / 2;

  const uint32_t pll1m = 1; // Input prescaler (1 == divide  by 1)
  const uint32_t pll2m = 1; // Input prescaler (1 == divide  by 1)
  const uint32_t pll3m = 1; // Input prescaler (1 == divide  by 1)
  const uint32_t plln = 99;
  const uint32_t pllp = 1;
  const uint32_t pllq = 20;

  const uint32_t d1cpre  = 0;
  const uint32_t hpre    = 0xa; // Divide by 4
  const uint32_t d1ppre  = 0x4; // Divide by 2

  const uint32_t d2ppre1 = 0x4; // Divide by 2
  const uint32_t d2ppre2 = 0x4; // Divide by 2

  const uint32_t d3ppre  = 0x4; // Divide by 2

  int vos = VOS_LEVEL_1;

  set_flash_latency(axi_freq, vos);

  voltage_scaling(vos);
  int pllscr;

#if 1
  // Use external 8MHz clock
  // HSE osc bypass (no crystal)
  reg_set_bit(RCC_CR, 18);

  // HSE clock enable
  reg_set_bit(RCC_CR, 16);

  // Wait for stable HSE clock
  while(!(reg_rd(RCC_CR) & (1 << 17))) {}

  pllscr = 2; // HSE is PLL clock

#else
  // HSI Clock divide by 8   64 -> 8 MHz
  reg_set_bits(RCC_CR, 3, 2, 3);

  pllscr = 0; // HSI is PLL clock

#endif

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

  // Start PLL1
  reg_set_bit(RCC_CR, 24);

  // Wait for PLL1
  while(!(reg_rd(RCC_CR) & (1 << 25))) {}

  // Switch to PLL1
  reg_set_bits(RCC_CFGR, 0, 3, 3);
  while(((reg_rd(RCC_CFGR) >> 3) & 0x7) != 3) {}

  // Set MCO2 divider to divide by 10
  //  reg_set_bits(RCC_CFGR, 25, 4, 10);
}


unsigned int
clk_get_freq(uint16_t id)
{
  // TODO: Fix this incorrect hack
  return 25000000;
}

#include <stdio.h>
void
reset_peripheral(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 8), id & 0xff);
  reg_clr_bit(RCC_BASE + (id >> 8), id & 0xff);
}
