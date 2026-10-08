#include <mios/sys.h>
#include <mios/cli.h>

#include "stm32h7_reg.h"
#include "stm32h7_clk.h"

static uint32_t reset_reason;
static uint32_t reset_status_raw;

static void  __attribute__((constructor(102)))
stm32h7_get_reset_reason(void)
{
  uint32_t rr = reg_rd(RCC_RSR);
  reset_status_raw = rr;
  uint32_t n = 0;
  if(rr & (1 << 28)) {
    n |= RESET_REASON_WATCHDOG;
  }
  if(rr & (1 << 26)) {
    n |= RESET_REASON_WATCHDOG;
  }
  if(rr & (1 << 24)) {
    n |= RESET_REASON_SW_RESET;
  }
  if(rr & (1 << 23)) {
    n |= RESET_REASON_POWER_ON;
  }
  if(rr & (1 << 22)) {
    n |= RESET_REASON_EXT_RESET;
  }
  if(rr & (1 << 21)) {
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

static error_t
cmd_resetflags(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "Boot RCC_RSR: 0x%08x, reset reason mask: 0x%08x\n",
             (unsigned)reset_status_raw, (unsigned)reset_reason);
  return 0;
}

CLI_CMD_DEF_EXT("resetflags", cmd_resetflags, "", "Saved STM32H7 boot reset flags");
