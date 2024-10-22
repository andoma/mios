#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#include "stm32f4_reg.h"
#include "stm32f4_clk.h"
#include "stm32f4_uart.h"
#include "stm32f4_eth.h"

static void __attribute__((constructor(101)))
board_setup_clocks(void)
{
  stm32f4_init_pll(8);
}

static void __attribute__((constructor(110)))
board_init_console(void)
{
  static stm32_uart_stream_t console;
  stdio = stm32f4_uart_stream_init(&console, 3, 115200, GPIO_PD(8), GPIO_PD(9),
                                   UART_CTRLD_IS_PANIC, "console");
}

static const uint8_t stm32f439_nucleo144_ethernet_gpios[] =
  { GPIO_PA(1),
    GPIO_PA(2),
    GPIO_PA(7),
    GPIO_PG(11),
    GPIO_PG(13),
    GPIO_PB(13),
    GPIO_PC(1),
    GPIO_PC(4),
    GPIO_PC(5),
  };

static void __attribute__((constructor(800)))
board_init_late(void)
{
  stm32f4_eth_init(GPIO_UNUSED,
                   stm32f439_nucleo144_ethernet_gpios,
                   sizeof(stm32f439_nucleo144_ethernet_gpios),
                   NULL, 0, ETHPHY_MODE_RMII);
}

