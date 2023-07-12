#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>
#include <mios/cli.h>

#include "irq.h"

#include "nrf52_reg.h"
#include "nrf52_uart.h"
#include "nrf52_timer.h"
#include "nrf52_radio.h"

// Product: https://www.adafruit.com/product/3406
// Schematics: https://cdn-learn.adafruit.com/assets/assets/000/052/793/original/microcontrollers_revgsch.png

#define LED_RED  GPIO_P0(17)
#define LED_BLUE GPIO_P0(19)


static void __attribute__((constructor(110)))
board_init_console(void)
{
  stdio = nrf52_uart_init(115200, GPIO_P0(6), GPIO_P0(8),
                          UART_CTRLD_IS_PANIC);
}


__attribute__((noreturn))
static void *
blinker(void *arg)
{
  while(1) {
    gpio_set_output(LED_RED, 1);
    usleep(100000);
    gpio_set_output(LED_RED, 0);
    usleep(900000);
  }
}

static void
ble_status(int flags)
{
  gpio_set_output(LED_RED, !!(flags & NRF52_BLE_STATUS_CONNECTED));
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  gpio_conf_output(LED_RED, GPIO_PUSH_PULL,
                   GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_output(LED_BLUE, GPIO_PUSH_PULL,
                   GPIO_SPEED_LOW, GPIO_PULL_NONE);

  thread_create(blinker, NULL, 512, "blinker", 0, 0);

  nrf52_radio_ble_init("bluefruit", ble_status);
}
