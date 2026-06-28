#include <stdio.h>

#include <mios/io.h>
#include <mios/mcp.h>
#include <mios/type_macros.h>

#include "nrf54l_uart.h"

// nRF54L15 DK onboard debugger virtual serial ports (HW User Guide v1.0),
// both exposed as host ACM ports:
//   VCOM "UART_1": TXD P1.04, RXD P1.05 -> UARTE20 (PERI domain, SERIAL20=198)
//   VCOM "UART_0": TXD P0.00, RXD P0.01 -> UARTE30 (LP domain,   SERIAL30=260)
#define UARTE20_BASE 0x500c6000
#define SERIAL20_IRQ 198
#define UARTE30_BASE 0x50104000
#define SERIAL30_IRQ 260

// Board LEDs (active high). HW User Guide Table 5.
static const gpio_t leds[] = {
  GPIO_P2(9),   // LED0
  GPIO_P1(10),  // LED1
  GPIO_P2(7),   // LED2
  GPIO_P1(14),  // LED3
};


static void __attribute__((constructor(110)))
board_init_console(void)
{
  // Primary console (default main() runs the CLI on stdio). Ctrl-D panics.
  stdio = nrf54l_uart_init(UARTE20_BASE, SERIAL20_IRQ, 115200,
                           GPIO_P1(4), GPIO_P1(5), UART_CTRLD_IS_PANIC);
}


static void __attribute__((constructor(800)))
board_init_late(void)
{
  // Drive the LEDs as outputs (off); no animation.
  for(size_t i = 0; i < ARRAYSIZE(leds); i++)
    gpio_conf_output(leds[i], GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  // MCP (structured, HDLC-framed) on the other VCOM (UARTE30, P0.00/P0.01).
  stream_t *mcp = nrf54l_uart_init(UARTE30_BASE, SERIAL30_IRQ, 115200,
                                   GPIO_P0(0), GPIO_P0(1), 0);
  mcp_uart_create(mcp);
}
