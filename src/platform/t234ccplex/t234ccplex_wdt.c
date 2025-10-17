#include "reg.h"

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
         1000000);   // 1 second

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
