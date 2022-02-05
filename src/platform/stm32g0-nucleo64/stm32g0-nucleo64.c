#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>
#include <mios/cli.h>

#include "irq.h"

#include "stm32g0_clk.h"
#include "stm32g0_uart.h"
#include "stm32g0_i2c.h"

#define BLINK_GPIO  GPIO_PA(5) // Green led (User LD1)


static void __attribute__((constructor(101)))
board_init(void)
{
  stm32g0_init_pll();

  static stm32_uart_t console;
  stdio = stm32g0_uart_init(&console, 2, 115200,
                            stm32g0_uart_tx(2, GPIO_PA(2)),
                            stm32g0_uart_rx(2, GPIO_PA(3)),
                            UART_CTRLD_IS_PANIC);
}

struct {
  uint8_t instance;
  gpio_t scl;
  gpio_t sda;
  const char *info;
} i2c_mappings[] = {
  { 1, GPIO_PA(9),  GPIO_PA(10) },
  { 2, GPIO_PA(11), GPIO_PA(12) },
  { 1, GPIO_PB(6),  GPIO_PB(7)  },
  { 1, GPIO_PB(8),  GPIO_PB(9), "D14/D15"  },
  { 2, GPIO_PB(10), GPIO_PB(11)  },
  { 2, GPIO_PB(13), GPIO_PB(14)  },
};

static void
i2c_map_name(char out[static 20], uint8_t instance, gpio_t scl, gpio_t sda)
{
  snprintf(out, 20, "i2c%d@p%c%d,p%c%d",
           instance,
           'a' + (scl >> 4), scl & 0xf,
           'a' + (sda >> 4), sda & 0xf);
}

static error_t
cmd_i2c_makebus(cli_t *cli, int argc, char **argv)
{
  char tmp[20];

  if(argc == 2) {
    for(size_t i = 0; i < ARRAYSIZE(i2c_mappings); i++) {
      i2c_map_name(tmp,
                   i2c_mappings[i].instance,
                   i2c_mappings[i].scl,
                   i2c_mappings[i].sda);
      if(!strcmp(tmp, argv[1])) {
        i2c_t *i2c =
          stm32g0_i2c_create(i2c_mappings[i].instance,
                             i2c_mappings[i].scl,
                             i2c_mappings[i].sda,
                             GPIO_PULL_UP);
        if(i2c == NULL) {
          cli_printf(cli, "Valid bus but failed to create (busy?)\n");
          return ERR_BAD_STATE;
        } else {
          cli_printf(cli, "Created ok\n");
          return 0;
        }
      }
    }
  }

  cli_printf(cli, "Available busses: (SCL,SDA)\n");

  for(size_t i = 0; i < ARRAYSIZE(i2c_mappings); i++) {
    i2c_map_name(tmp,
                 i2c_mappings[i].instance,
                 i2c_mappings[i].scl,
                 i2c_mappings[i].sda);
    cli_printf(cli, "\t%-20s %s\n", tmp,
               i2c_mappings[i].info ?: "");
  }

  return 0;
}

CLI_CMD_DEF("i2c-makebus", cmd_i2c_makebus);



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
  gpio_conf_output(BLINK_GPIO, GPIO_PUSH_PULL,
                   GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_set_output(BLINK_GPIO, 1);

  task_create(blinker, NULL, 256, "blinker", 0, 0);

#if 0 // Uncomment to start full duplex mbus on USART1
  stm32g0_mbus_uart_create(1, 115200,
                           stm32g0_uart_tx(1, GPIO_PA(9)),
                           stm32g0_uart_rx(1, GPIO_PA(10)),
                           GPIO_UNUSED,
                           10, NULL, 1, 0);
#endif
}

