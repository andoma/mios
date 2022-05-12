#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>
#include <mios/cli.h>
#include <mios/suspend.h>

#include "irq.h"

#include "stm32wb_uart.h"
#include "stm32wb_spi.h"
#include "stm32wb_clk.h"
#include "stm32wb_pwr.h"

#define LED_BLUE  GPIO_PB(5)
#define LED_GREEN GPIO_PB(0)
#define LED_RED   GPIO_PB(1)

static void __attribute__((constructor(101)))
board_init(void)
{
  stm32wb_use_hse();

  static stm32_uart_t console;

  stdio = stm32wb_uart_init(&console, STM32WB_INSTANCE_USART1, 115200,
                            GPIO_PB(6), GPIO_PB(7),
                            UART_CTRLD_IS_PANIC);

}



__attribute__((noreturn))
static void *
blinker(void *arg)
{
  while(1) {
    wakelock_acquire();
    gpio_set_output(LED_RED, 1);
    usleep(333333);
    gpio_set_output(LED_RED, 0);
    gpio_set_output(LED_GREEN, 1);
    usleep(333333);
    gpio_set_output(LED_GREEN, 0);
    gpio_set_output(LED_BLUE, 1);
    usleep(333333);
    gpio_set_output(LED_BLUE, 0);
    wakelock_release();
    usleep(333333);
  }
}

static int suspend_enabled;

static void
wakeup_button(void *arg)
{
  if(suspend_enabled) {
    wakelock_acquire();
    suspend_enabled = 0;
  }
}


static void __attribute__((constructor(800)))
platform_init_late(void)
{
  if(1) {
    reg_wr(PWR_SCR, 0xffffffff);
    gpio_conf_irq(GPIO_PC(13), GPIO_PULL_UP, wakeup_button, NULL,
                  GPIO_FALLING_EDGE, IRQ_LEVEL_IO);

    suspend_enable();
    wakelock_acquire();
  }

  gpio_conf_output(LED_BLUE, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_output(LED_GREEN, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_output(LED_RED, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  task_create(blinker, NULL, 512, "blinker", 0, 0);
}


static error_t
cmd_suspend(cli_t *cli, int argc, char **argv)
{
  if(suspend_enabled)
    return ERR_BAD_STATE;

  suspend_enabled = 1;
  wakelock_release();

  return 0;
}

CLI_CMD_DEF("suspend", cmd_suspend);

