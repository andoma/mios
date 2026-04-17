#include <mios/sys.h>

#include "stm32n6_reg.h"
#include "stm32n6_clk.h"

// RCC_RSR bit layout (RM0486 §14.10.8):
//   bit 30 LPWRRSTF  — Illegal Stop or Standby
//   bit 28 WWDGRSTF  — Window watchdog reset
//   bit 26 IWDGRSTF  — Independent watchdog reset
//   bit 24 SFTRSTF   — Software reset (SYSRESETREQ)
//   bit 23 PORRSTF   — POR/PDR
//   bit 22 PINRSTF   — Pin (NRST) reset
//   bit 21 BORRSTF   — Brownout reset
//   bit 17 LCKRSTF   — CPU lockup
//   bit 16 RMVF      — write-1 to clear all flags

#define RCC_RSR_LPWRRSTF  (1u << 30)
#define RCC_RSR_WWDGRSTF  (1u << 28)
#define RCC_RSR_IWDGRSTF  (1u << 26)
#define RCC_RSR_SFTRSTF   (1u << 24)
#define RCC_RSR_PORRSTF   (1u << 23)
#define RCC_RSR_PINRSTF   (1u << 22)
#define RCC_RSR_BORRSTF   (1u << 21)
#define RCC_RSR_LCKRSTF   (1u << 17)
#define RCC_RSR_RMVF      (1u << 16)

static uint32_t reset_reason;

static void __attribute__((constructor(102)))
stm32n6_get_reset_reason(void)
{
  uint32_t rr = reg_rd(RCC_RSR);
  uint32_t n = 0;

  if(rr & RCC_RSR_LPWRRSTF)
    n |= RESET_REASON_LOW_POWER_RESET;
  if(rr & (RCC_RSR_IWDGRSTF | RCC_RSR_WWDGRSTF))
    n |= RESET_REASON_WATCHDOG;
  if(rr & RCC_RSR_SFTRSTF)
    n |= RESET_REASON_SW_RESET;
  if(rr & RCC_RSR_PORRSTF)
    n |= RESET_REASON_POWER_ON;
  if(rr & RCC_RSR_PINRSTF)
    n |= RESET_REASON_EXT_RESET;
  if(rr & RCC_RSR_BORRSTF)
    n |= RESET_REASON_BROWNOUT;
  if(rr & RCC_RSR_LCKRSTF)
    n |= RESET_REASON_CPU_LOCKUP;

  // Clear the flags so the next boot shows only the new cause
  reg_wr(RCC_RSR, RCC_RSR_RMVF);

  reset_reason = n;
}

reset_reason_t
sys_get_reset_reason(void)
{
  return reset_reason;
}
