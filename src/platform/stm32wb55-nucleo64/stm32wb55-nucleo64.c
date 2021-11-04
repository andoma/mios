#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>
#include <mios/cli.h>

#include "irq.h"

#include "stm32wb_uart.h"
#include "stm32wb_spi.h"
#include "stm32wb_clk.h"

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


static void *
blinker(void *arg)
{
  while(1) {
    gpio_set_output(LED_RED, 1);
    usleep(333333);
    gpio_set_output(LED_RED, 0);
    gpio_set_output(LED_GREEN, 1);
    usleep(333333);
    gpio_set_output(LED_GREEN, 0);
    gpio_set_output(LED_BLUE, 1);
    usleep(333333);
    gpio_set_output(LED_BLUE, 0);
  }
  return NULL;
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  gpio_conf_output(LED_BLUE, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_output(LED_GREEN, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_output(LED_RED, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  task_create(blinker, NULL, 512, "blinker", 0, 0);
}
