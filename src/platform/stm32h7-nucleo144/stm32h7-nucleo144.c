#include <unistd.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/task.h>


#include "stm32h7.h"
#include "stm32h7_clk.h"
#include "stm32h7_uart.h"

#define BLINK_GPIO  GPIO_PB(0) // Green led (User LD1)

static void __attribute__((constructor(101)))
board_setup_clocks(void)
{
  stm32h7_init_pll();
}



// Virtual COM port (USB)
// Connected to USART 3 - PD8 (Our TX)  PD9 (Our RX)
static stm32h7_uart_t console;

static void __attribute__((constructor(110)))
board_init_console(void)
{
  clk_enable(CLK_GPIOA);
  clk_enable(CLK_GPIOB);
  clk_enable(CLK_GPIOC);
  clk_enable(CLK_GPIOD);

  stm32h7_uart_init(&console, 3, 115200, GPIO_PD(8), GPIO_PD(9),
                    UART_CTRLD_IS_PANIC);
  stdio = &console.stream;

  gpio_conf_af(GPIO_PC(9), 0, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);

  gpio_set_output(BLINK_GPIO, 1);
  gpio_conf_output(BLINK_GPIO, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);
}


static void *
blinker(void *arg)
{
  while(1) {
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 0);
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 1);
  }
  return NULL;
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  task_create(blinker, NULL, 512, "blinker", 0, 0);
}
