#include <mios/cli.h>
#include <mios/sys.h>

#include "stm32g0_reg.h"
#include "stm32g0_clk.h"

static uint8_t reset_reason;

static void  __attribute__((constructor(102)))
stm32f4_get_reset_reason(void)
{
  uint32_t rr = reg_rd(RCC_CSR);

  if(rr & (1 << 31)) {
    reset_reason = RESET_REASON_LOW_POWER_RESET;
  } else if(rr & (1 << 30)) {
    reset_reason = RESET_REASON_WATCHDOG;
  } else if(rr & (1 << 29)) {
    reset_reason = RESET_REASON_WATCHDOG;
  } else if(rr & (1 << 28)) {
    reset_reason = RESET_REASON_SW_RESET;
  } else if(rr & (1 << 27)) {
    reset_reason = RESET_REASON_POWER_ON;
  } else if(rr & (1 << 26)) {
    reset_reason = RESET_REASON_EXT_RESET;
  } else {
    reset_reason = 0;
  }
  reg_wr(RCC_CSR, rr | (1 << 23));
}

reset_reason_t
sys_get_reset_reason(void)
{
  return reset_reason;
}
