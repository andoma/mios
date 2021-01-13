#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#include "stm32g0_clk.h"
#include "stm32g0_uart.h"

#define BLINK_GPIO  GPIO_PA(5) // Green led (User LD1)

static stm32g0_uart_t console;

uint32_t xval;

static void __attribute__((constructor(101)))
board_init_console(void)
{
  int p = irq_forbid(IRQ_LEVEL_DMA);
  irq_permit(p);

  clk_enable(CLK_GPIOA);

  gpio_set_output(BLINK_GPIO, 1);
  gpio_conf_output(BLINK_GPIO, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  stm32g0_uart_init(&console, 2, 115200, GPIO_PA(2), GPIO_PA(3),
                    UART_CTRLD_IS_PANIC);
  stdio = &console.stream;
}
