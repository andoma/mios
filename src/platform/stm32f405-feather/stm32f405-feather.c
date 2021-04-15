#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"

#include "pinmap.h"
#include "stm32f4.h"
#include "stm32f4_clk.h"
#include "stm32f4_uart.h"

#define NEOPIX_GPIO GPIO_PC(0)
#define BLINK_GPIO  GPIO_PC(1) // Red led close to USB connection

static stm32f4_uart_t console;

static void __attribute__((constructor(110)))
board_init_console(void)
{
  stm32f4_uart_init(&console, 6, 115200, GPIO_PC(6), GPIO_PC(7),
                    UART_CTRLD_IS_PANIC);
  stdio = &console.stream;
}


static void __attribute__((constructor(101)))
board_setup_clocks(void)
{
  stm32f4_init_pll(8);
}



#include "cpu.h"

static inline void __attribute__((always_inline))
cyclewait(uint32_t ref, uint32_t t)
{
  while(cpu_cycle_counter() - ref < t) {
  }
}



static void
neopix_init(void)
{
  gpio_set_output(NEOPIX_GPIO, 0);
  gpio_conf_output(NEOPIX_GPIO, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);
}

void
neopix(uint8_t r, uint8_t g, uint8_t b)
{
  int word = (g << 24) | (r << 16) | (b << 8);
  const gpio_t pin = NEOPIX_GPIO;

  int q = irq_forbid(IRQ_LEVEL_ALL);
  uint32_t ref = cpu_cycle_counter();
  uint32_t t0 = 0;

  for(int j = 0; j < 24; j++, word <<= 1) {

    cyclewait(ref, t0);
    gpio_set_output(pin, 1);

    int o = word < 0 ? 134 : 67;
    cyclewait(ref, t0 + o);
    gpio_set_output(pin, 0);
    t0 += 201;
  }
  irq_permit(q);
}

#include <mios/cli.h>
#include <stdlib.h>

static int
cmd_neopix(cli_t *c, int argc, char **argv)
{
  if(argc < 4)
    return -1;
  neopix(atoi(argv[1]),atoi(argv[2]),atoi(argv[3]));
  return 0;
}

CLI_CMD_DEF("neopix", cmd_neopix);


static void *
blinker(void *arg)
{
  gpio_conf_output(BLINK_GPIO, GPIO_PUSH_PULL,
                   GPIO_SPEED_LOW, GPIO_PULL_NONE);

  gpio_set_output(BLINK_GPIO, 1); // Red LED

  neopix_init();
  usleep(20);
  neopix(0,0,10);

  while(1) {
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 0); // Red LED
    usleep(500000);
    gpio_set_output(BLINK_GPIO, 1); // Red LED
  }
  return NULL;
}

static void __attribute__((constructor(800)))
platform_init_late(void)
{
  task_create(blinker, NULL, 512, "blinker", 0, 0);
}



void
platform_panic(void)
{
  neopix(10,0,10);
}
