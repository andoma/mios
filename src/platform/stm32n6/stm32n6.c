#include "stm32n6_uart.h"
#include "stm32n6_reg.h"
#include "stm32n6_pwr.h"
#include "stm32n6_clk.h"
#include "stm32n6_usb.h"

#include <malloc.h>
#include <stdio.h>

#include <mios/timer.h>
#include <mios/sys.h>

#include <usb/usb.h>



static void __attribute__((constructor(101)))
board_init_clk(void)
{
  stm32n6_init_pll(48000000);
}

static void __attribute__((constructor(102)))
board_init_console(void)
{
  static stm32_uart_stream_t console;
  stdio = stm32n6_uart_init(&console, 1, 115200, GPIO_PE(5), GPIO_PE(6),
                            UART_CTRLD_IS_PANIC, "console");
}

static void  __attribute__((constructor(120)))
stm32n6_init(void)
{
  heap_add_mem(HEAP_START_EBSS, 0x24100000,
               MEM_TYPE_DMA | MEM_TYPE_VECTOR_TABLE | MEM_TYPE_CODE, 20);

  heap_add_mem(0x20000400, 0x20020000, MEM_TYPE_LOCAL, 30);

  reg_set_bit(PWR_SVMCR3, 26); // VDDIO3VRSEL (1.8V)
  reg_set_bit(PWR_SVMCR3, 9);  // VDDIO3SV
  reg_set_bit(PWR_SVMCR3, 1);  // VDDIO3VMEN

  reg_set_bit(PWR_SVMCR3, 10);  // USB33SV
  reg_set_bit(PWR_SVMCR3, 2);   // USB33VMEN
}

static timer_t blinky_timer;
static char blinky_phase;

static int blinky_duration = 500000;

static void
blinky_callback(void *arg, uint64_t now)
{
  blinky_phase = !blinky_phase;
  gpio_set_output(GPIO_PG(10), blinky_phase);
  timer_arm_abs(&blinky_timer, now + blinky_duration);
}


static void  __attribute__((constructor(500)))
led_init(void)
{
  gpio_conf_output(GPIO_PG(10), GPIO_PUSH_PULL, GPIO_SPEED_LOW,
                   GPIO_PULL_NONE);

  gpio_set_output(GPIO_PG(10), 1);

  blinky_timer.t_cb = blinky_callback;
  timer_arm_abs(&blinky_timer, blinky_duration);
}


static void  __attribute__((constructor(900)))
late_init(void)
{
  struct usb_interface_queue q;
  STAILQ_INIT(&q);

  // Expose a serial-port that is a normal console to this OS
  usb_cdc_create_shell(&q);

  stm32n6_otghs_create(0x6666, 0x0500, "Lonelycoder", "stm32n6-dk", &q);
}


const struct serial_number
sys_get_serial_number(void)
{
  struct serial_number sn;
  sn.data = (const void *)0x46009014;
  sn.len = 12;
  return sn;
}
