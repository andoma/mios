#include "reg.h"

#include <mios/ghook.h>
#include <mios/cli.h>

#include <stdio.h>

#define NUM_TIMERS 16

#define TMR_BASE(x) (0x2080000 + 0x10000 + 0x10000 * (x))
#define WDT_BASE(x) (0x2080000 + 0x10000 + 0x10000 * NUM_TIMERS + 0x10000 * (x))

#define TMR_CONFIG 0x000
#define TMR_STATUS 0x004
#define TMR_CSSR   0x008

#define WDT_CONFIG   0x0
#define WDT_STATUS   0x4
#define WDT_COMMAND  0x8
#define WDT_UNLOCK   0xc

static void __attribute__((constructor(101)))
wdt_init(void)
{
  reg_wr(TMR_BASE(0) + TMR_CSSR, 0); // µsec base

  reg_wr(TMR_BASE(0) + TMR_CONFIG,
         (1 << 31) | // Enable
         (1 << 30) | // Periodic
         1000000);   // 1 Hz

  reg_wr(WDT_BASE(0) + WDT_CONFIG, 0x00710010);  // 1 second period
  reg_wr(WDT_BASE(0) + WDT_COMMAND, 1); // Restart watchdog
}


__attribute__((noreturn))
void
cpu_idle(void)
{
  while(1) {
    asm volatile("wfi");
     // Restart watchdog
     reg_wr(WDT_BASE(0) + WDT_COMMAND, 1);
  }
}



static void
wdt_shutdown_hook(ghook_type_t type, const char *reason)
{
  if(type != GHOOK_SYSTEM_SHUTDOWN)
    return;

  if(reg_rd(TMR_BASE(0) + TMR_CONFIG) == 0)
    return;
  printf("Stretching watchdog timeout to 50s\n");
  reg_wr(WDT_BASE(0) + WDT_UNLOCK, 0xc45a);
  reg_wr(WDT_BASE(0) + WDT_COMMAND, 2);

  reg_wr(WDT_BASE(0) + WDT_CONFIG, 0x007100a0);  // 10s period (x5 until HW reboot)
  reg_wr(WDT_BASE(0) + WDT_COMMAND, 1); // Restart watchdog
}

GHOOK(wdt_shutdown_hook);

static error_t
cmd_wdogoff(cli_t *cli, int argc, char **argv)
{
  reg_wr(WDT_BASE(0) + WDT_UNLOCK, 0xc45a);
  reg_wr(WDT_BASE(0) + WDT_COMMAND, 2);
  reg_wr(TMR_BASE(0) + TMR_CONFIG, 0);
  return 0;
}

CLI_CMD_DEF("wdogoff", cmd_wdogoff);
