#include "reg.h"

#include "t234spe_wdt.h"

void
cpu_idle(void)
{
  // Config watchdog - Power-on-reset on expire
  reg_wr(WDT_AON_BASE + WDT_CONFIG, 0x00710000);

  while(1) {
    asm volatile("wfi");
    reg_wr(WDT_AON_BASE + WDT_COMMAND, 1); // Restart watchdog
  }
}
