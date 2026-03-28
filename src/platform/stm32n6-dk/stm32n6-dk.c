#include <unistd.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/task.h>
#include "stm32n6_clk.h"
#include "stm32n6_pwr.h"
#include "stm32n6_usb.h"
#include "stm32n6_eth.h"
#include "stm32n6_uart.h"

#include <usb/usb.h>

#define BLINK_GPIO GPIO_PG(10) // Red LED (LD2)

static void __attribute__((constructor(101)))
board_init_clk(void)
{
  stm32n6_init_pll(48000000);

  stm32n6_pwr_vddio3_enable(STM32N6_VDDIO_1V8);
  stm32n6_pwr_usb33_enable();
}

static void __attribute__((constructor(102)))
board_init_console(void)
{
  static stm32_uart_stream_t console;
  stdio = stm32n6_uart_init(&console, 1, 115200, GPIO_PE(5), GPIO_PE(6),
                            UART_CTRLD_IS_PANIC, "console");
}

__attribute__((noreturn))
static void *
blinker(void *arg)
{
  gpio_conf_output(BLINK_GPIO, GPIO_PUSH_PULL, GPIO_SPEED_LOW,
                   GPIO_PULL_NONE);
  while(1) {
    gpio_set_output(BLINK_GPIO, 1);
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 0);
    usleep(500000);
  }
}

static void __attribute__((constructor(800)))
board_init_late(void)
{
  thread_create(blinker, NULL, 512, "blinker", 0, 0);

  struct usb_interface_queue q;
  STAILQ_INIT(&q);

  usb_cdc_create_shell(&q);

  stm32n6_otghs_create(0x6666, 0x0500, "Lonelycoder", "stm32n6-dk", &q);

  stm32n6_eth_init(GPIO_UNUSED, GPIO_PD(12), GPIO_PD(1),
                   1, ETHPHY_MODE_RGMII);
}
