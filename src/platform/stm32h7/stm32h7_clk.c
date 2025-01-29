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
 PLL1p will be configured to run at CPU_SYSCLK_MHZ
 This also generates the bus clocks as follows:
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


 For a duty cycle close to 50%, DIV[P/Q/R]x divisor shall be even.
 Thus the value in the register should be odd

*/


static uint32_t ahb_freq = 64000000; // Default after reset
static uint32_t apb_freq = 64000000; // Default after reset
static uint32_t pll2q_freq;
static uint32_t pll2p_freq;


#define MHZ 1000000

const char *
stm32h7_init_pll(unsigned int hse_freq, uint32_t flags)
{
  int pll2_freq = 400 * MHZ;

  reg_clr_bit(PWR_CR3, 2); // Disable SMPS
  while(reg_get_bit(PWR_CSR1, 13) == 0) {}

  const int sysclk_freq_mhz = CPU_SYSCLK_MHZ;
  const int sysclk_freq = sysclk_freq_mhz * MHZ;

  const int axi_freq = sysclk_freq / 2;

  ahb_freq = sysclk_freq / 2;
  apb_freq = sysclk_freq / 4;

  int vos;
  if(sysclk_freq_mhz > 400) {
    vos = VOS_LEVEL_0;
  } else if(sysclk_freq_mhz > 300) {
    vos = VOS_LEVEL_1;
  } else if(sysclk_freq_mhz > 170) {
    vos = VOS_LEVEL_2;
  } else {
    vos = VOS_LEVEL_3;
  }

  int pllscr;

  if(hse_freq) {

    if(flags & STM32H7_HSE_NO_XTAL) {
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
    hse_freq = 8 * MHZ;
  }

  const uint32_t d1cpre  = 0;
  const uint32_t hpre    = 0x8; // Divide by 2
  const uint32_t d1ppre  = 0x4; // Divide by 2

  const uint32_t d2ppre1 = 0x4; // Divide by 2
  const uint32_t d2ppre2 = 0x4; // Divide by 2

  const uint32_t d3ppre  = 0x4; // Divide by 2

  // Configure clock dividers for domains
  reg_wr(RCC_D1CFGR,
         (hpre << 0) |
         (d1ppre << 4) |
         (d1cpre << 8));

  reg_wr(RCC_D2CFGR,
         (d2ppre1 << 4) |
         (d2ppre2 << 8));

  reg_wr(RCC_D3CFGR, d3ppre << 4);

  const uint32_t pll1m = 1; // Input prescaler (1 == divide  by 1)
  const uint32_t pll2m = 1; // Input prescaler (1 == divide  by 1)
  const uint32_t pll3m = 1; // Input prescaler (1 == divide  by 1)

  const uint32_t pll1n = (sysclk_freq / hse_freq) - 1;
  const uint32_t pll1p = 0;
  const uint32_t pll1q = 0;
  const uint32_t pll1r = 0;

  pll2q_freq = 100 * MHZ;
  pll2p_freq = 200 * MHZ;

  const uint32_t pll2n = (pll2_freq / hse_freq) - 1;
  const uint32_t pll2p = (pll2_freq / pll2p_freq) - 1;
  const uint32_t pll2q = (pll2_freq / pll2q_freq) - 1;
  const uint32_t pll2r = 0;


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
         (1 << 19) | // PLL2(p) enable
         (1 << 20) | // PLL2(q) enable
         0);

  // Write multiplier and output divider
  reg_wr(RCC_PLL1DIVR, (pll1r << 24) | (pll1q << 16) | (pll1p << 9) | pll1n);
  reg_wr(RCC_PLL2DIVR, (pll2r << 24) | (pll2q << 16) | (pll2p << 9) | pll2n);

  set_flash_latency(axi_freq, vos);

  voltage_scaling(vos);

  // Start PLL1
  reg_set_bit(RCC_CR, 24);
  // Wait for PLL1
  while(!(reg_rd(RCC_CR) & (1 << 25))) {}

  // Start PLL2
  reg_set_bit(RCC_CR, 26);
  // Wait for PLL2
  while(!(reg_rd(RCC_CR) & (1 << 27))) {}

  // Switch to PLL1
  reg_set_bits(RCC_CFGR, 0, 3, 3);
  while(((reg_rd(RCC_CFGR) >> 3) & 0x7) != 3) {}

  // Turn on HSI48 (USB and RNG)
  reg_set_bit(RCC_CR, 12);
  while(!reg_get_bit(RCC_CR, 13)) {}

  reg_wr(RCC_D2CCIP1R,
         (0b10 << 28)  | // FDCAN clocked from pll2q
         (0b001 << 12) | // SPI1,2,3 clocked from pll2p
         0);

  reg_wr(RCC_D2CCIP2R,
         (0b11 << 20) | // USB clocked from HSI48
         0);

  return NULL;
}


unsigned int
clk_get_freq(uint16_t id)
{
  uint8_t bus = id >> 8;

  switch(id) {
  case CLK_SPI1:
  case CLK_SPI2:
  case CLK_SPI3:
  case CLK_ADC12:
    return pll2p_freq;

  case CLK_FDCAN:
    return pll2q_freq;
  }

  switch(bus) {
  case RCC_AHB1RSTR & 0xff:
  case RCC_AHB1ENR & 0xff:
  case RCC_AHB4ENR & 0xff:
  case RCC_AHB1LPENR & 0xff:
    return ahb_freq;

  case RCC_APB1LRSTR & 0xff:
  case RCC_APB1HRSTR & 0xff:
  case RCC_APB1LENR & 0xff:
  case RCC_APB1HENR & 0xff:
  case RCC_APB2ENR & 0xff:
  case RCC_APB4ENR & 0xff:
    return apb_freq;
  default:
    panic("Can't resolve clkid 0x%x into freq", id);
  }
}


void
reset_peripheral(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 8), id & 0xff);
  reg_clr_bit(RCC_BASE + (id >> 8), id & 0xff);
}
