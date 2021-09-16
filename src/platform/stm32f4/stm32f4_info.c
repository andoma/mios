#include <mios/cli.h>
#include <mios/sys.h>

#include "stm32f4_reg.h"

#define RCC_CSR     0x40023874

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
  } else if(rr & (1 << 25)) {
    reset_reason = RESET_REASON_BROWNOUT;
  } else {
    reset_reason = 0;
  }
  reg_wr(RCC_CSR, rr | (1 << 24));
}

reset_reason_t
sys_get_reset_reason(void)
{
  return reset_reason;
}

const struct serial_number
sys_get_serial_number(void)
{
  struct serial_number sn;
  sn.data = (const void *)0x1fff7a10;
  sn.len = 12;
  return sn;
}
