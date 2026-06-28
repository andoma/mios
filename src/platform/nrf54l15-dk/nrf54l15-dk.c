#include <unistd.h>
#include <stdio.h>

#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>
#include <mios/type_macros.h>

#include "nrf54l_uart.h"

// nRF54L15 DK onboard debugger virtual serial ports (HW User Guide v1.0):
//   VCOM "UART_1": TXD P1.04, RXD P1.05  -> UARTE20 (PERI domain, SERIAL20)
//   VCOM "UART_0": TXD P0.00, RXD P0.01  -> UARTE30 (LP domain,   SERIAL30)
// We use UARTE20 as the console. SERIAL30 (260) is outside our IRQ table.
#define UARTE20_BASE 0x500c6000
#define SERIAL20_IRQ 198

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
  stdio = nrf54l_uart_init(UARTE20_BASE, SERIAL20_IRQ, 115200,
                           GPIO_P1(4), GPIO_P1(5), UART_CTRLD_IS_PANIC);
}


__attribute__((noreturn))
static void *
blinker(void *arg)
{
  // Knight-Rider style sweep across the four LEDs
  int i = 0;
  int dir = 1;
  while(1) {
    for(size_t j = 0; j < ARRAYSIZE(leds); j++)
      gpio_set_output(leds[j], j == (size_t)i);

    usleep(120000);

    i += dir;
    if(i == ARRAYSIZE(leds) - 1 || i == 0)
      dir = -dir;
  }
}


static void __attribute__((constructor(800)))
board_init_late(void)
{
  for(size_t i = 0; i < ARRAYSIZE(leds); i++)
    gpio_conf_output(leds[i], GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  thread_create(blinker, NULL, 512, "blinker", 0, 0);
}
