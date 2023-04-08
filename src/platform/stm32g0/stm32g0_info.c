#include <mios/cli.h>
#include <mios/sys.h>

#include "stm32g0_reg.h"
#include "stm32g0_clk.h"

static uint32_t reset_reason;

static void  __attribute__((constructor(102)))
stm32f4_get_reset_reason(void)
{
  uint32_t rr = reg_rd(RCC_CSR);

  uint32_t n = 0;
  if(rr & (1 << 31)) {
    n |= RESET_REASON_LOW_POWER_RESET;
  } else if(rr & (1 << 30)) {
    n |= RESET_REASON_WATCHDOG;
  } else if(rr & (1 << 29)) {
    n |= RESET_REASON_WATCHDOG;
  } else if(rr & (1 << 28)) {
    n |= RESET_REASON_SW_RESET;
  } else if(rr & (1 << 27)) {
    n |= RESET_REASON_POWER_ON;
  } else if(rr & (1 << 26)) {
    n |= RESET_REASON_EXT_RESET;
  }
  reg_wr(RCC_CSR, rr | (1 << 23));
  reset_reason = n;
}

reset_reason_t
sys_get_reset_reason(void)
{
  return reset_reason;
}
