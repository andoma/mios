#include "nrf52_wdt.h"
#include "nrf52_reg.h"

#include <mios/timer.h>
#include <mios/task.h>


static timer_t wdog_timer;
static task_t wdog_task;

static int nokick;

static void
wdog_timer_cb(void *opaque, uint64_t expire)
{
  timer_arm_abs(&wdog_timer, expire + 10000000);
  softirq_trig(&wdog_task);
}

static void
wdog_task_cb(task_t *t)
{
  if(nokick)
    return;
  reg_wr(WDT_RR(0), WDT_RESET_VALUE);
}


static void __attribute__((constructor(130)))
nrf52_wdt_init(void)
{
  wdog_timer.t_cb = wdog_timer_cb;
  wdog_task.t_run = wdog_task_cb;

  reg_wr(WDT_CRV, 20 * 32768); // in seconds
  reg_wr(WDT_CONFIG, 1);       // Keep watchdog running while CPU is asleep
  reg_wr(WDT_RREN, 1);         // RR[0] enable
  reg_wr(WDT_TASKS_START, 1);

  timer_arm_abs(&wdog_timer, 1000000);
}


#include <mios/cli.h>


static error_t
cmd_nokick(cli_t *cli, int argc, char **argv)
{
  nokick = 1;
  return 0;
}

CLI_CMD_DEF("nokick", cmd_nokick);
