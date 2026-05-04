#include "stm32n6_reg.h"
#include "stm32n6_wdog.h"

#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#define IWDG_IRQ 18  // STM32N6: WDGLS (IWDG early wake-up interrupt)

// LSI ≈ 32 kHz, PR=6 → /256 → 128 Hz tick.
//   RLR  = 640 → ~5.0 s timeout / hardware reset
//   EWIT = 384 → IWDG EWI fires when counter ≤ 383, i.e. ~2.0 s after refresh
// After the EWI handler refreshes IWDG (KR=0xAAAA) the panic has another full
// ~5 s before the hardware reset hits — plenty of time for a backtrace.
#define IWDG_PR_VAL    6
#define IWDG_RLR_VAL   640
#define IWDG_EWIT_VAL  384
#define IWDG_EWCR_INIT ((1u << 15) | IWDG_EWIT_VAL) // EWIE=1
#define IWDG_ICR_EWIC  (1u << 15)

void
irq_18(void)
{
  reg_wr(IWDG_ICR, IWDG_ICR_EWIC); // ack EWI
  reg_wr(IWDG_KR, 0xAAAA);         // refresh — buy another reload period
  thread_t *t = thread_current();
  panic("IWDG soft watchdog: task=\"%s\"", t ? t->t_name : "?");
}

static void __attribute__((constructor(132)))
stm32n6_iwdg_init(void)
{
  reg_wr(IWDG_KR, 0x5555);           // unlock
  reg_wr(IWDG_RLR, IWDG_RLR_VAL);
  reg_wr(IWDG_PR, IWDG_PR_VAL);
  reg_wr(IWDG_EWCR, IWDG_EWCR_INIT);
  reg_wr(IWDG_KR, 0xAAAA);           // refresh + lock
  reg_wr(IWDG_KR, 0xCCCC);           // start (no-op if bootloader did)

  irq_enable(IWDG_IRQ, IRQ_LEVEL_HIGH);
}
