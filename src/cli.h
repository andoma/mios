#pragma once

#include <stdint.h>

#define CLI_LINE_BUF_SIZE 32



typedef struct cli {

  int16_t cl_pos;

  void (*cl_printf)(struct cli *cli, const char *fmt, ...);

  // This includes a terminating 0 at all times
  char cl_buf[CLI_LINE_BUF_SIZE];

} cli_t;




typedef struct cli_cmd {
  const char *cmd;
  int (*dispatch)(cli_t *cli, int argc, char **argv);
} cli_cmd_t;

#define CLI_GLUE(a, b) a ## b
#define CLI_JOIN(a, b) CLI_GLUE(a, b)

#define CLI_CMD_DEF(name, fn) \
  cli_cmd_t CLI_JOIN(cli, __LINE__) __attribute__ ((section ("clicmd"))) = { name, fn};

#define cli_printf(cli, fmt...) (cli)->cl_printf(cli, fmt)

void cli_input_char(cli_t *cl, char c);

void cli_prompt(cli_t *cl);

void cli_console(void);
