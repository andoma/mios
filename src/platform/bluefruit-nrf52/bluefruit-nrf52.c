#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#include "nrf52_uart.h"


static void __attribute__((constructor(110)))
board_init_console(void)
{
  stdio = nrf52_uart_init(115200, GPIO_P0(6), GPIO_P0(8), 0);
}
