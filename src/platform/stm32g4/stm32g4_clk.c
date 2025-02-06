#include <mios/mios.h>
#include <assert.h>

#include "stm32g4_clk.h"
#include "stm32g4_pwr.h"

static uint32_t apb1clock;
static uint32_t apb2clock;

#define FLASH_ACR   0x40022000


static uint8_t g_hse_freq;
static uint8_t g_p_freq;
static uint8_t g_q_freq;

int
clk_get_freq(uint16_t id)
{
  uint32_t r;

  switch(id) {
  case CLK_FDCAN:
    return g_q_freq * 1000000;

  default:
    break;
  }

  switch(id >> 8) {
  default:
    panic("clk_get_speed() invalid id: 0x%x", id);
  case RCC_APB1ENR1 & 0xff:
  case RCC_APB1ENR2 & 0xff:
    r = apb1clock;
    break;
  case RCC_APB2ENR & 0xff:
    r = apb2clock;
    break;
  case RCC_AHB2ENR & 0xff:
    return CPU_SYSTICK_RVR;
  }
  if(!r)
    panic("Clock 0x%x not initialized", id);
  return r;
}


void
stm32g4_reinit_pll(void)
{
  int pll_m = 4; // Internal 16MHz / 4
  uint32_t pllcfgr = 2;

#if CPU_SYSCLK_MHZ > 150
  reg_clr_bit(PWR_CR5, 8);
#endif

  // D-CACHE I-CACHE PREFETCH, 4 wait states
  reg_wr(FLASH_ACR, (1 << 18) | 0x704);

  // D-CACHE I-CACHE PREFETCH, 5 wait states
  reg_wr(FLASH_ACR, (1 << 18) | 0x705);

  reg_wr(RCC_CFGR,
         (0x4 << 11) | // APB2 (High speed) prescaler = 2
         (0x5 << 8));  // APB1 (Low speed)  prescaler = 4

  if(g_hse_freq) {
    reg_set_bit(RCC_CR, 16); // HSEON
    while(!(reg_rd(RCC_CR) & (1 << 17))) {} // Wait for external oscillator
    pll_m = g_hse_freq / 4;
    pllcfgr = 3;
  }

  const int fvco = CPU_SYSCLK_MHZ * 2;

  if(g_p_freq) {
    pllcfgr |= (1 << 16); // PLLP Enable

    int pdiv = fvco / g_p_freq;
    assert(pdiv >= 2 && pdiv < 32);
    pllcfgr |= (pdiv << 27);
  }

  g_q_freq = fvco / 6;

  reg_wr(RCC_PLLCFGR,
         (1 << 24) |                   // PLLR enable
         (1 << 20) |                   // PLLQ Enable
         (0b10 << 21) |                // PLLQ = FVCO / 6
         pllcfgr |
         ((pll_m - 1) << 4) |          // input division
         ((CPU_SYSCLK_MHZ / 2) << 8) | // PLL multiplication
         (0 << 25)                     // PLL sys clock division (0 == /2)
         );

  reg_set_bit(RCC_CR, 24);

  while(!(reg_rd(RCC_CR) & (1 << 25))) {} // Wait for pll

  reg_set_bits(RCC_CFGR, 0, 2, 3); // Use PLL as system clock

  while((reg_rd(RCC_CFGR) & 0xc) != 0xc) {}

  reg_set_bits(RCC_CCIPR, 24, 2, 0b01); // FDCAN clocked from PLLQ
}



void
stm32g4_init_pll(uint8_t hse_freq, uint8_t p_freq)
{
  reg_wr(RCC_ICSCR, 0x40000000); // Reset clock trim

  apb1clock = CPU_SYSTICK_RVR / 4;
  apb2clock = CPU_SYSTICK_RVR / 2;

  g_hse_freq = hse_freq;
  g_p_freq = p_freq;
  stm32g4_reinit_pll();
}

void
stm32g4_enable_backup_domain(void)
{
  if(reg_get_bit(PWR_CR1, 8))
    return;

  clk_enable(CLK_PWR);
  reg_set_bit(PWR_CR1, 8);
  if(reg_get_bit(RCC_BDCR, 15)) {
    return;
  }
  reg_set_bit(RCC_BDCR, 16);
  reg_clr_bit(RCC_BDCR, 16);
}

void
stm32g4_lse_enable(void)
{
  stm32g4_enable_backup_domain();

  reg_set_bit(RCC_BDCR, 0);
  while((reg_rd(RCC_BDCR) & 2) == 0) {}
}


void
clk_enable_hsi48(void)
{
  reg_wr(RCC_CRRCR, 1);
  while(!(reg_rd(RCC_CRRCR) & (1 << 1))) {} // Wait for clock to become ready
}

void
stm32g4_deinit_pll(void)
{
  // Use HSI16 as system clock
  reg_set_bits(RCC_CFGR, 0, 2, 1);
  while((reg_rd(RCC_CFGR) & 0x4) != 0x4) {}

  reg_clr_bit(RCC_CR, 24);
  while((reg_rd(RCC_CR) & (1 << 25))) {} // Wait for pll to stop

  reg_wr(RCC_CFGR, 0x5); // Reset value (all prescalers reset)
  reg_wr(RCC_PLLCFGR, 0x1000);
}


void
stm32g4_deinit_clk(void)
{
  // Reset all peripheral (except flash)
  reg_wr(RCC_AHB1RSTR, 0x101f);
  reg_wr(RCC_AHB2RSTR, 0x50f607f);
  reg_wr(RCC_AHB3RSTR, 0x101);
  reg_wr(RCC_APB1RSTR1, 0xd2fec13f);
  reg_wr(RCC_APB1RSTR2, 0x103);
  reg_wr(RCC_APB2RSTR, 0x437f801);

  // Disable all peripheral clocks (write reset values to registers)
  reg_wr(RCC_AHB1ENR, 0x100);
  reg_wr(RCC_AHB2ENR, 0x1); // Keep SYSCFG on
  reg_wr(RCC_AHB3ENR, 0x0);
  reg_wr(RCC_APB1ENR1, 0x400);
  reg_wr(RCC_APB1ENR2, 0x0);
  reg_wr(RCC_APB2ENR, 0x0);

  // Release resets
  reg_wr(RCC_AHB1RSTR, 0);
  reg_wr(RCC_AHB2RSTR, 0);
  reg_wr(RCC_AHB3RSTR, 0);
  reg_wr(RCC_APB1RSTR1, 0);
  reg_wr(RCC_APB1RSTR2, 0);
  reg_wr(RCC_APB2RSTR, 0);

  // Turn off HSI48
  reg_wr(RCC_CRRCR, 0);
  while((reg_rd(RCC_CRRCR) & (1 << 1))) {} // Wait for clock to stop

  stm32g4_deinit_pll();

  reg_wr(RCC_CCIPR, 0x0);
}
