#include "stm32n6_clk.h"
#include "stm32n6_pwr.h"

#include <stddef.h>
#include <stdio.h>


static uint32_t apb_clk = 32000000;
uint32_t stm32n6_hse_freq;

// The timers are not clocked from the APB bus like the rest of the
// APB1/APB2 peripherals: they are fed from sys_bus_ck through TIMPRE
// (RM0486 14.10.5, timg_ck), which at the reset default of TIMPRE = 0
// is sys_bus_ck undivided — twice the APB clock with HPRE at /2.
static const uint16_t timer_clk_ids[] = {
  CLK_TIM1,  CLK_TIM2,  CLK_TIM3,  CLK_TIM4,  CLK_TIM5,  CLK_TIM6,
  CLK_TIM7,  CLK_TIM8,  CLK_TIM9,  CLK_TIM10, CLK_TIM11, CLK_TIM12,
  CLK_TIM13, CLK_TIM14, CLK_TIM15, CLK_TIM16, CLK_TIM17, CLK_TIM18,
};

unsigned int
clk_get_freq(uint16_t id)
{
  for(size_t i = 0; i < sizeof(timer_clk_ids) / sizeof(timer_clk_ids[0]); i++) {
    if(timer_clk_ids[i] != id)
      continue;

    // PPREx are pinned to divide-by-1 (required by erratum), so
    // apb_clk is sys_bus2_ck and sys_bus_ck is that shifted back up
    // by HPRE.
    const uint32_t cfgr2 = reg_rd(RCC_CFGR2);
    const uint32_t hpre = (cfgr2 >> 20) & 7;
    const uint32_t timpre = (cfgr2 >> 24) & 3;
    return (apb_clk << hpre) >> timpre;
  }
  return apb_clk;
}

/*

  sysa_ck   ---------- CPU                                  600 (VOS=0)
                                                            800 (VOS=1)
  sysb_ck   +--------- AXI                                  400
            |
            +-[TIMPRE] Timers
            |
            +-[HPRE]---+----------- AHB                     200
                       +----------- hclk0,1,2,3,4,5,m       200
                       +-[PPRE1] -- APB1                    200
                       +-[PPRE2] -- APB2                    200
                       +-[PPRE4] -- APB4                    200
                       +-[PPRE5] -- APB5                    200


    Note: STM32N6 Errata says that PPREx must be fixed to divide-by-1

 */


static void
enable_ic(int unit, int divider, int source)
{
  reg_wr(RCC_ICxCFGR(unit), ((divider - 1) << 16) | (source << 28));
  reg_set_bit(RCC_DIVENR, unit - 1); // Use set-register instead
}


const char *
stm32n6_init_pll(unsigned int hse_freq)
{
  stm32n6_hse_freq = hse_freq;

  reg_set_bit(RCC_CR, 4); // Enable HSE (External XTAL)
  while(reg_get_bit(RCC_SR, 4) == 0) {
  }

  // Configure PLL1 HSE@48MHZ * 50 == 2400MHz
  int divm = 1;
  int divn = 50;
  //  int divp = 1;

  reg_wr(RCC_PLLxCFGR2(1), 0);  // Clear fractional divider (FSBL leaves 0x800000)

  reg_wr(RCC_PLLxCFGR1(1),
         (0b010 << 28) | // Select HSE
         (divm  << 20) |
         (divn  << 8)  |
         0);

  //  reg_set_bits(RCC_PLLxCFGR3(1), 24, 3, divp);

  int cpudiv = 3; // CPU @ 800MHz
  int axidiv = 6; // SYS @ 400MHz

  enable_ic(1,  cpudiv, 0);
  enable_ic(2,  axidiv, 0);
  enable_ic(6,  axidiv, 0);
  enable_ic(11, axidiv, 0);
  enable_ic(15, 100,    0); // 24MHz For OTG (USB)

  // Start PLL1
  reg_set_bit(RCC_CR, 8);
  while(reg_get_bit(RCC_SR, 8) == 0) {
  }

  // Switch CPU to use ic1
  reg_set_bits(RCC_CFGR1, 16, 2, 3);
  while(((reg_rd(RCC_CFGR1) >> 20) & 3) != 3) { }

  // SYS from ic2,ic6 and ic11

  apb_clk = 200000000;

  int hpre = 1;  // divide by 2
  int ppre = 0;  // divide by 1

  reg_wr(RCC_CFGR2,
         (hpre << 20) | // HPRE
         (ppre << 16) |
         (ppre << 12) |
         (ppre << 4) |
         (ppre << 0));

  // Switch SYS to use ic2,6,11
  reg_set_bits(RCC_CFGR1, 24, 2, 3);
  while(((reg_rd(RCC_CFGR1) >> 28) & 3) != 3) { }

  // OTGPHY1SEL == 2 (ic15_ck)
  reg_set_bits(RCC_CCIPR6, 12, 2, 2);

  // OTGPHY2SEL == 2 (ic15_ck)
  reg_set_bits(RCC_CCIPR6, 20, 2, 2);

  // Force XSPI1's kernel clock to hclk5 (200 MHz), the source the NOR driver's
  // prescaler assumes. On a cold boot XSPI1SEL comes up as per_ck (measured =1)
  // instead of hclk5, so without this the NOR runs at the wrong rate and reads
  // back corrupt; a serial/DFU boot already leaves it on hclk5 (=0).
  // (RCC_CCIPR6 XSPI1SEL[1:0]: 00 = hclk5, 01 = per_ck, 10 = ic3, 11 = ic4.)
  reg_set_bits(RCC_CCIPR6, 0, 2, 0);

  // Enable all peripheral clocks during sleep mode (LPENR registers).
  // Without this, WFI stops peripheral clocks and interrupts can't fire.
  for(int i = 0; i < 14; i++)
    reg_wr(RCC_BASE + 0x284 + i * 4, 0xFFFFFFFF);

  return NULL;
}
