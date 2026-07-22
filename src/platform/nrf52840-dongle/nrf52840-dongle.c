#include <unistd.h>
#include <stdio.h>

#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include <usb/usb.h>

#include "nrf52840_usb.h"
#include "nrf_sdc.h"

// Nordic nRF52840 Dongle (PCA10059). LEDs are active low.
//   LD1 green: P0.06     LD2 RGB: red P0.08, blue P0.12 (green is on P1.09,
//   which the port-0-only GPIO layer does not reach).
//
// There is no software path back into the factory Open Bootloader: it enters
// DFU only on a pin reset (the RST button), ignoring GPREGRET, so no dfu()
// hook or USB DFU-runtime interface is provided. Reflash with `make flash`
// (PLATFORM=nrf52840-dongle), tapping RST when it asks.
#define LED_GREEN GPIO_P0(6)


__attribute__((noreturn))
static void *
blinker(void *arg)
{
  while(1) {
    gpio_set_output(LED_GREEN, 0); // on
    usleep(50000);
    gpio_set_output(LED_GREEN, 1); // off
    usleep(950000);
  }
}


static void __attribute__((constructor(800)))
board_init_late(void)
{
  gpio_conf_output(LED_GREEN, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_set_output(LED_GREEN, 1); // off

  thread_create(blinker, NULL, 512, "blinker", 0, 0);

  nrf_ble_init("mios-dongle");
}


static void __attribute__((constructor(1000)))
board_init_usb(void)
{
  struct usb_interface_queue q;
  STAILQ_INIT(&q);

  usb_cdc_create_shell(&q);   // CDC-ACM console + shell over USB
  usb_mcp_create(&q, 0x01);   // MCP (structured CLI/mem access) over USB

  nrf52840_usb_create(0x6666, 0x0011, "Lonelycoder",
                      "mios-nrf52840-dongle", &q);
}
