#include <mios/mios.h>

#include "stm32f4_clk.h"
#include "stm32f4_flash.h"
#include "cpu.h"
#include "systick.h"

#define APB1_CLK_BITS 0x1ff
#define APB2_CLK_BITS 0x70003

int
clk_get_freq(uint16_t id)
{
  uint32_t r;

  uint32_t bit = (1 << (id & 0x1f));

  const uint32_t apb1clock = CPU_SYSTICK_RVR / 4;
  const uint32_t apb2clock = CPU_SYSTICK_RVR / 2;

  switch(id >> 8) {
  default:
    panic("clk_get_speed() invalid id: 0x%x", id);
  case CLK_APB1:
    r = apb1clock;
    if(bit & APB1_CLK_BITS)
      r *= 2;
    break;
  case CLK_APB2:
    r = apb2clock;
    if(bit & APB2_CLK_BITS)
      r *= 2;
    break;
  }
  if(!r)
    panic("Clock 0x%x not initialized", id);
  return r;
}


static volatile unsigned int * const SYST_RVR = (unsigned int *)0xe000e014;

void
systick_timepulse(void)
{
  uint32_t c = cpu_cycle_counter();
  static uint32_t prev;
  uint32_t delta = c - prev;
  prev = c;

  static uint32_t lp;

  if(delta > CPU_SYSTICK_RVR - 10000 && delta < CPU_SYSTICK_RVR + 10000) {
    if(lp) {
      lp = (lp * 3 + 2 + delta) / 4;
      *SYST_RVR = (lp + (HZ / 2)) / HZ - 1;
    } else {
      lp = delta;
    }
  } else {
    lp = 0;
  }
}


#ifdef CPU_SYSCLK_MHZ

void
stm32f4_init_pll(int hse_freq)
{
  int pll_m = 8;
  uint32_t pllcfgr = 0;
  reg_wr(FLASH_ACR, 0x705); // D-CACHE I-CACHE PREFETCH, 5 wait states

  reg_wr(RCC_CFGR,
         (0x4 << 13) | // APB2 (High speed) prescaler = 2
         (0x5 << 10)); // APB1 (Low speed)  prescaler = 4

  if(hse_freq) {
    reg_set_bit(RCC_CR, 16); // HSEON
    while(!(reg_rd(RCC_CR) & (1 << 17))) {} // Wait for external oscillator
    pll_m = hse_freq / 2;
    pllcfgr |= (1 << 22);
  }

  // Clock for USB should be 48MHz
  const uint32_t pll_q = CPU_SYSCLK_MHZ * 2 / 48;

  reg_wr(RCC_PLLCFGR,
         pllcfgr |
         (pll_m << 0) |          // input division
         (CPU_SYSCLK_MHZ << 6) | // PLL multiplication
         (0 << 16) |             // PLL sys clock division (0 == /2)
         (pll_q << 24));         // PLL usb clock division

  reg_set_bit(RCC_CR, 24);

  while(!(reg_rd(RCC_CR) & (1 << 25))) {} // Wait for pll

  reg_set_bits(RCC_CFGR, 0, 2, 2); // Use PLL as system clock

  while((reg_rd(RCC_CFGR) & 0xc) != 0x8) {}
}

#endif

void
reset_peripheral(uint16_t id)
{
  reg_set_bit(RCC_BASE + (id >> 8), id & 0xff);
  reg_clr_bit(RCC_BASE + (id >> 8), id & 0xff);
}
