#include <mios/sys.h>

#include "stm32h7_reg.h"
#include "stm32h7_clk.h"

static uint32_t reset_reason;

static void  __attribute__((constructor(102)))
stm32h7_get_reset_reason(void)
{
  uint32_t rr = reg_rd(RCC_RSR);
  uint32_t n = 0;
  if(rr & (1 << 28)) {
    n |= RESET_REASON_WATCHDOG;
  } else if(rr & (1 << 26)) {
    n |= RESET_REASON_WATCHDOG;
  } else if(rr & (1 << 24)) {
    n |= RESET_REASON_SW_RESET;
  } else if(rr & (1 << 23)) {
    n |= RESET_REASON_POWER_ON;
  } else if(rr & (1 << 22)) {
    n |= RESET_REASON_EXT_RESET;
  } else if(rr & (1 << 21)) {
    n |= RESET_REASON_BROWNOUT;
  }
  reg_wr(RCC_RSR, rr | (1 << 16));
  reset_reason = n;
}

reset_reason_t
sys_get_reset_reason(void)
{
  return reset_reason;
}
