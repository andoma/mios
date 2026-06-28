#include <mios/timer.h>
#include <mios/task.h>
#include <mios/cli.h>

#include "nrf54l_reg.h"

// WDT30 (LP domain). Clocked from the 32.768 kHz LFCLK, which the WDT
// starts itself (the LFRC) when it runs. timeout[s] = (CRV + 1) / 32768.
#define WDT_BASE        0x50108000

#define WDT_TASKS_START (WDT_BASE + 0x000)
#define WDT_CRV         (WDT_BASE + 0x504)
#define WDT_RREN        (WDT_BASE + 0x508)
#define WDT_CONFIG      (WDT_BASE + 0x50c)
#define WDT_RR(x)       (WDT_BASE + 0x600 + (x) * 4)

#define WDT_RELOAD_REQUEST 0x6e524635

#define WDT_TIMEOUT_SEC 20
#define WDT_KICK_PERIOD 10000000 // 10 s, in microseconds

static timer_t wdog_timer;
static task_t wdog_task;
static int nokick;


// Reload from a task, not directly from the timer IRQ, so that a wedged
// scheduler (which can't run this task) lets the watchdog fire.
static void
wdog_task_cb(task_t *t)
{
  if(nokick)
    return;
  reg_wr(WDT_RR(0), WDT_RELOAD_REQUEST);
}


static void
wdog_timer_cb(void *opaque, uint64_t expire)
{
  timer_arm_abs(&wdog_timer, expire + WDT_KICK_PERIOD);
  task_run(&wdog_task);
}


static void __attribute__((constructor(131)))
nrf54l_wdt_init(void)
{
  wdog_timer.t_cb = wdog_timer_cb;
  wdog_task.t_run = wdog_task_cb;

  reg_wr(WDT_CRV, WDT_TIMEOUT_SEC * 32768 - 1);
  reg_wr(WDT_CONFIG, 1); // SLEEP: keep running while the CPU is asleep
  reg_wr(WDT_RREN, 1);   // enable reload request register RR[0]
  reg_wr(WDT_TASKS_START, 1);

  timer_arm_abs(&wdog_timer, WDT_KICK_PERIOD);
}


static error_t
cmd_wdogoff(cli_t *cli, int argc, char **argv)
{
  nokick = 1;
  cli_printf(cli, "Watchdog kicking disabled; reset in <= %d s\n",
             WDT_TIMEOUT_SEC);
  return 0;
}

CLI_CMD_DEF("wdogoff", cmd_wdogoff);
