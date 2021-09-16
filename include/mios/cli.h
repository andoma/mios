#pragma once

#include <stdint.h>
#include <stdio.h>

#define CLI_LINE_BUF_SIZE 32

typedef struct cli {

  void (*cl_printf)(struct cli *cli, const char *fmt, ...);
  int (*cl_getc)(struct cli *cli, int wait);

  struct stream *cl_stream;

  int16_t cl_pos;

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
  static cli_cmd_t CLI_JOIN(cli, __LINE__) __attribute__ ((used, section("clicmd"))) = { name, fn};

#define cli_printf(cli, fmt...) stprintf((cli)->cl_stream, fmt);

int cli_getc(cli_t *cli, int wait);

int cli_on_stream(struct stream *s, char promptchar);

void cli_console(char promptchar);
