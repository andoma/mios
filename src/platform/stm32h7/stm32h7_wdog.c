#include "stm32h7_clk.h"
#include "stm32h7_reg.h"
#include "stm32h7_wdog.h"

#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

// ---- IWDG: hard reset backstop --------------------------------------------
// LSI ≈ 32 kHz, PR=6 → /256 → 128 Hz tick. RLR = 384 → ~3 s timeout.
#define IWDG_PR_VAL    6
#define IWDG_RLR_VAL   (128 * 3)

static void __attribute__((constructor(104)))
stm32h7_iwdg_init(void)
{
  reg_wr(IWDG_KR, 0x5555);
  reg_wr(IWDG_RLR, IWDG_RLR_VAL);
  reg_wr(IWDG_PR, IWDG_PR_VAL);
  reg_wr(IWDG_KR, 0xAAAA);
  reg_wr(IWDG_KR, 0xCCCC);
}

// ---- LPTIM5: pure-software soft watchdog ----------------------------------
// Clocked from LSI (~32 kHz) with PRESC=/4 → 8 kHz counter. ARR=20000 →
// ~2.5 s nominal timeout, just under IWDG's 3 s. Both share LSI so the
// ratio (~0.83) is constant across PVT — LPTIM always fires before IWDG.
// RSTARE=1 means any read of LPTIM5_CNT resets the counter, so the idle
// loop just reads once per WFI to kick. ARRM IRQ → panic.
#define LPTIM5_IRQ      141
#define LPTIM5_PRESC    (2u << 9)  // CFGR PRESC=010 → /4
#define LPTIM5_ARR_VAL  20000

#define LPTIM_ICR_ARRMCF (1u << 1)
#define LPTIM_ISR_ARROK  (1u << 4)
#define LPTIM_IER_ARRMIE (1u << 1)
#define LPTIM_CR_ENABLE  (1u << 0)
#define LPTIM_CR_CNTSTRT (1u << 2)
#define LPTIM_CR_RSTARE  (1u << 4)

#define RCC_CSR         (RCC_BASE + 0x074)
#define RCC_CSR_LSIRDY  (1u << 1)
#define LPTIM345SEL_LSI (4u << 13)
#define LPTIM345SEL_MSK (7u << 13)

void
irq_141(void)
{
  reg_wr(LPTIM5_ICR, LPTIM_ICR_ARRMCF);
  thread_t *t = thread_current();
  panic("LPTIM5 soft watchdog: task=\"%s\"", t ? t->t_name : "?");
}

// Run just before multitasking_mark (4999) so all driver init has finished
// before the ~2.5 s timeout starts ticking.
static void __attribute__((constructor(4990)))
stm32h7_lptim5_wdog_init(void)
{
  reg_set_bit(RCC_CSR, 0);                     // LSION=1
  while(!(reg_rd(RCC_CSR) & RCC_CSR_LSIRDY)) {}

  uint32_t ccipr = reg_rd(RCC_D3CCIPR);
  reg_wr(RCC_D3CCIPR, (ccipr & ~LPTIM345SEL_MSK) | LPTIM345SEL_LSI);

  reg_set_bit(RCC_APB4ENR, 12);                // LPTIM5EN
  (void)reg_rd(RCC_APB4ENR);

  reg_wr(LPTIM5_CR, 0);                        // ENABLE=0 — required to write CFGR/IER
  reg_wr(LPTIM5_CFGR, LPTIM5_PRESC);           // PRESC=/4, internal clock, sw start
  reg_wr(LPTIM5_IER, LPTIM_IER_ARRMIE);

  reg_wr(LPTIM5_CR, LPTIM_CR_ENABLE);          // ARR & RSTARE require ENABLE=1
  reg_wr(LPTIM5_ARR, LPTIM5_ARR_VAL);
  while(!(reg_rd(LPTIM5_ISR) & LPTIM_ISR_ARROK)) {}

  reg_wr(LPTIM5_CR, LPTIM_CR_ENABLE | LPTIM_CR_RSTARE | LPTIM_CR_CNTSTRT);

  irq_enable(LPTIM5_IRQ, IRQ_LEVEL_HIGH);
}
