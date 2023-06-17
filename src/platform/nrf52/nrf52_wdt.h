#pragma once

#define WDT_BASE         0x40010000

#define WDT_TASKS_START (WDT_BASE + 0x000)

#define WDT_CRV         (WDT_BASE + 0x504)
#define WDT_RREN        (WDT_BASE + 0x508)
#define WDT_CONFIG      (WDT_BASE + 0x50c)

#define WDT_RR(x)       (WDT_BASE + 0x600 + (x) * 4)

#define WDT_RESET_VALUE  0x6E524635
