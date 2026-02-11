#pragma once

#include <stdint.h>
#include <stdio.h>
#include <mios/error.h>

#define CLI_LINE_BUF_SIZE 48
#define CLI_MAX_ARGC 10

typedef struct cli {

  struct stream *cl_stream;

  char *cl_argv[CLI_MAX_ARGC];

  int16_t cl_pos;

  // This includes a terminating 0 at all times
  char cl_buf[CLI_LINE_BUF_SIZE];

} cli_t;




typedef struct cli_cmd {
  const char *cmd;
  error_t (*dispatch)(cli_t *cli, int argc, char **argv);
} cli_cmd_t;

#define CLI_GLUE(a, b) a ## b
#define CLI_JOIN(a, b) CLI_GLUE(a, b)

#define CLI_CMD_DEF(name, fn) \
  static const cli_cmd_t CLI_JOIN(cli, __LINE__) __attribute__ ((used, section("clicmd."#name))) = { name, fn};

#define cli_printf(cli, fmt...) stprintf((cli)->cl_stream, fmt)

int cli_getc(cli_t *cli, int wait);

int cli_on_stream(struct stream *s, char promptchar);

error_t cli_dispatch_command(cli_t *c, int argc, char *argv[]);

void cli_console(char promptchar);

#define cli_flush(cli) stream_write((cli)->cl_stream, NULL, 0, 0)
