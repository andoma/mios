#include <unistd.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/task.h>

#include "stm32h7_clk.h"
#include "stm32h7_usb.h"
#include "stm32h7_eth.h"
#include "stm32h7_uart.h"

#include <usb/usb.h>

#define BLINK_GPIO  GPIO_PB(0) // Green led (User LD1)

static void __attribute__((constructor(102)))
board_setup_clocks(void)
{
  stm32h7_init_pll(8000000, STM32H7_HSE_NO_XTAL);

  static stm32_uart_stream_t console;
  stdio = stm32h7_uart_init(&console, 3, 115200, GPIO_PD(8), GPIO_PD(9),
                            UART_CTRLD_IS_PANIC, "console");

}



// Virtual COM port (USB)
// Connected to USART 3 - PD8 (Our TX)  PD9 (Our RX)

static void __attribute__((constructor(110)))
board_init_console(void)
{

  gpio_conf_af(GPIO_PC(9), 0, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);

  gpio_conf_output(BLINK_GPIO, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);
}

__attribute__((noreturn))
static void *
blinker(void *arg)
{
  while(1) {
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 0);
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 1);
  }
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  thread_create(blinker, NULL, 512, "blinker", 0, 0);

  struct usb_interface_queue q;
  STAILQ_INIT(&q);

  // Expose a serial-port that is a normal console to this OS
  usb_cdc_create_shell(&q);

  stm32h7_otghs_create(0x6666, 0x0500, "Lonelycoder", "stm32h7-nucleo144", &q);

  stm32h7_eth_init();
}
