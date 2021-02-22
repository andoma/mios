#include <mios/mios.h>

#include "stm32h7_clk.h"
#include "stm32h7.h"


#define FLASH_ACR 0x52002000


#define PWR_BASE              0x58024800

#define PWR_CR3        (PWR_BASE + 0x0c)
#define PWR_D3CR       (PWR_BASE + 0x18)



#define RCC_BASE              0x58024400

#define RCC_CR         (RCC_BASE + 0x00)
#define RCC_CFGR       (RCC_BASE + 0x10)

#define RCC_D1CFGR     (RCC_BASE + 0x18)
#define RCC_D2CFGR     (RCC_BASE + 0x1c)
#define RCC_D3CFGR     (RCC_BASE + 0x20)
#define RCC_PLLCKSELR  (RCC_BASE + 0x28)
#define RCC_PLLCFGR    (RCC_BASE + 0x2c)
#define RCC_PLL1DIVR   (RCC_BASE + 0x30)


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

void
stm32h7_init_pll(void)
{
  // Run at 400MHz

  int sysclk_freq = 400;
  int axi_freq = sysclk_freq / 2;

  const uint32_t pllm = 1; // Input prescaler (1 == divide  by 1)
  const uint32_t plln = 49;
  const uint32_t pllp = 0;
  const uint32_t pllq = 20;

  const uint32_t d1cpre  = 0;
  const uint32_t hpre    = 0x8; // Divide by 2
  const uint32_t d1ppre  = 0x4; // Divide by 2

  const uint32_t d2ppre1 = 0x4; // Divide by 2
  const uint32_t d2ppre2 = 0x4; // Divide by 2

  const uint32_t d3ppre  = 0x4; // Divide by 2

  int vos = VOS_LEVEL_1;

  set_flash_latency(axi_freq, vos);

  voltage_scaling(vos);

  // HSI Clock divide by 8   64 -> 8 MHz
  reg_set_bits(RCC_CR, 3, 2, 3);

  reg_wr(RCC_D1CFGR,
         (hpre << 0) |
         (d1ppre << 4) |
         (d1cpre << 8));

  reg_wr(RCC_D2CFGR,
         (d2ppre1 << 4) |
         (d2ppre2 << 8));

  reg_wr(RCC_D3CFGR, d3ppre << 4);


  // Set prescaler for PLL1
  reg_wr(RCC_PLLCKSELR, pllm << 4);

  // PLL1 input rage (8-16 MHz) - DIVP1 output enable
  reg_wr(RCC_PLLCFGR, (3 << 2) | (1 << 16) | (1 << 17));

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
  reg_set_bits(RCC_CFGR, 25, 4, 10);
}


unsigned int
clk_get_freq(uint16_t id)
{
  // TODO: Fix this hack
  return 100000000;
}
