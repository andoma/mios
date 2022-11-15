#include <mios/mios.h>

#include "stm32g4_clk.h"

static uint32_t apb1clock;
static uint32_t apb2clock;

#define FLASH_ACR   0x40022000
#define RCC_CR      0x40021000
#define RCC_CFGR    0x40021008
#define RCC_PLLCFGR 0x4002100c


int
clk_get_freq(uint16_t id)
{
  uint32_t r;
  switch(id >> 8) {
  default:
    panic("clk_get_speed() invalid id: 0x%x", id);
  case RCC_APB1ENR1:
  case RCC_APB1ENR2:
    r = apb1clock;
    break;
  case RCC_APB2ENR:
    r = apb2clock;
    break;
  case RCC_AHB2ENR:
    return CPU_SYSTICK_RVR;
  }
  if(!r)
    panic("Clock 0x%x not initialized", id);
  return r;
}



void
stm32g4_init_pll(int hse_freq)
{
  int pll_m = 4; // Internal 16MHz / 4
  uint32_t pllcfgr = 2;

  // D-CACHE I-CACHE PREFETCH, 5 wait states
  reg_wr(FLASH_ACR, (1 << 18) | 0x705);

  apb1clock = CPU_SYSTICK_RVR / 4;
  apb2clock = CPU_SYSTICK_RVR / 2;

  reg_wr(RCC_CFGR,
         (0x4 << 11) | // APB2 (High speed) prescaler = 2
         (0x5 << 8));  // APB1 (Low speed)  prescaler = 4

  if(hse_freq) {
    reg_set_bit(RCC_CR, 16); // HSEON
    while(!(reg_rd(RCC_CR) & (1 << 17))) {} // Wait for external oscillator
    pll_m = hse_freq / 4;
    pllcfgr = 3;
  }

  // Clock for USB should be 48MHz
  const uint32_t pll_q = CPU_SYSCLK_MHZ * 2 / 48;

  reg_wr(RCC_PLLCFGR,
         (1 << 24) | // PLLR enable
         pllcfgr |
         ((pll_m - 1) << 4) |          // input division
         ((CPU_SYSCLK_MHZ / 2) << 8) | // PLL multiplication
         (0 << 25) |             // PLL sys clock division (0 == /2)
         (pll_q << 21));         // PLL usb clock division

  reg_set_bit(RCC_CR, 24);

  while(!(reg_rd(RCC_CR) & (1 << 25))) {} // Wait for pll

  reg_set_bits(RCC_CFGR, 0, 2, 3); // Use PLL as system clock

  while((reg_rd(RCC_CFGR) & 0xc) != 0xc) {}
}
