#include <stdlib.h>
#include <string.h>

#include <mios/cli.h>
#include <mios/io.h>

#ifdef GPIO // Currently only STM32 has this macro defined.

static error_t
cmd_gpio_read(cli_t *cli, int argc, char **argv)
{
  if(argc != 3) {
    cli_printf(cli, "gpio-read <block> <pin>\n");
    return ERR_INVALID_ARGS;
  }
  int port = (argv[1][0] | 32) - 'a';
  int pin = atoi(argv[2]);

  
  cli_printf(cli, "input %c%d: %d\n", port + 'A', pin, gpio_get_input(GPIO(port, pin)));
  return 0;
}

static error_t
cmd_gpio_write(cli_t *cli, int argc, char **argv)
{
  if(argc != 4) {
    cli_printf(cli, "gpio-write <block> <pin> <value>\n");
    return ERR_INVALID_ARGS;
  }
  int port = (argv[1][0] | 32) - 'a';
  int pin = atoi(argv[2]);
  int val = atoi(argv[3]);
  
  gpio_set_output(GPIO(port, pin), !!val);
  
  cli_printf(cli, "set %c%d: %d\n", port + 'A', pin, gpio_get_input(GPIO(port, pin)));
  return 0;
}


static error_t
cmd_gpio_conf(cli_t *cli, int argc, char **argv)
{
  if(argc != 4) {
    cli_printf(cli, "gpio-conf <block> <pin> <in/out>\n");
    return ERR_INVALID_ARGS;
  }
  int port = (argv[1][0] | 32) - 'a';
  int pin = atoi(argv[2]);

  if (!strcmp(argv[3], "in")) {
    gpio_conf_input(GPIO(port, pin), GPIO_PULL_NONE);
  } else if(!strcmp(argv[3], "out")) {
    gpio_conf_output(GPIO(port, pin), GPIO_PUSH_PULL,
			  GPIO_SPEED_LOW, GPIO_PULL_NONE);
  } else {
    cli_printf(cli, "gpio-conf: in or out\n");
    return ERR_INVALID_ARGS;
  }

  return 0;
}

CLI_CMD_DEF("gpio-get", cmd_gpio_read);
CLI_CMD_DEF("gpio-set", cmd_gpio_write);
CLI_CMD_DEF("gpio-conf", cmd_gpio_conf);

#endif
