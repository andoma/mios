#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#include "nrf52_uart.h"


#define LED_RED  GPIO_P0(17)
#define LED_BLUE GPIO_P0(19)


static void __attribute__((constructor(110)))
board_init_console(void)
{
  stdio = nrf52_uart_init(115200, GPIO_P0(6), GPIO_P0(8), 0);
}


__attribute__((noreturn))
static void *
blinker(void *arg)
{
  gpio_conf_output(LED_RED, GPIO_PUSH_PULL,
                   GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_output(LED_BLUE, GPIO_PUSH_PULL,
                   GPIO_SPEED_LOW, GPIO_PULL_NONE);

  while(1) {
    gpio_set_output(LED_RED, 0);
    gpio_set_output(LED_BLUE, 1);
    usleep(500000);
    gpio_set_output(LED_RED, 1);
    gpio_set_output(LED_BLUE, 0);
    usleep(500000);
  }
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  thread_create(blinker, NULL, 512, "blinker", 0, 0);
}
