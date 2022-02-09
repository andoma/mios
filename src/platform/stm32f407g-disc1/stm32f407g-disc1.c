#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#include "stm32f4_reg.h"
#include "stm32f4_clk.h"
#include "stm32f4_uart.h"

static stm32_uart_t console;

static void __attribute__((constructor(110)))
board_init_console(void)
{
  stm32f4_uart_init(&console, 2, 115200, GPIO_PA(2), GPIO_PA(3),
                    UART_CTRLD_IS_PANIC);
  stdio = &console.stream;
}


static void __attribute__((constructor(101)))
board_setup_clocks(void)
{
  // Use external 8MHz crystal
  stm32f4_init_pll(8);

  // 4 LEDS
  for(int i = 0; i < 4; i++) {
    gpio_conf_output(GPIO_PD(i + 12), GPIO_PUSH_PULL,
                     GPIO_SPEED_LOW, GPIO_PULL_NONE);
  }
  // Blue is always on
  gpio_set_output(GPIO_PD(15), 1);
}


__attribute__((noreturn))
static void *
blinker(void *arg)
{
  // Blinking green
  while(1) {
    gpio_set_output(GPIO_PD(12), 1);
    usleep(500000);
    gpio_set_output(GPIO_PD(12), 0);
    usleep(500000);
  }
}


static void __attribute__((constructor(800)))
platform_init_late(void)
{
  task_create(blinker, NULL, 512, "blinker", 0, 0);
}

