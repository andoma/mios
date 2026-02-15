#pragma once

#include <stdint.h>
#include <stdio.h>
#include <mios/error.h>

#define ENABLE_CLI_EXTENDED_HELP

typedef struct cli {

  struct stream *cl_stream;

} cli_t;




typedef struct cli_cmd {
  const char *cmd;
  error_t (*dispatch)(cli_t *cli, int argc, char **argv);
#ifdef ENABLE_CLI_EXTENDED_HELP
  const char *synopsis; // Example "<bus> <address>" (Help for arguments)
  const char *brief; // "Show information about foo" (Shown in CLI help)
#endif
} cli_cmd_t;

#define CLI_GLUE(a, b) a ## b
#define CLI_JOIN(a, b) CLI_GLUE(a, b)

#define CLI_CMD_DEF(name, fn) \
  static const cli_cmd_t CLI_JOIN(cli, __LINE__) __attribute__ ((used, section("clicmd."#name))) = { name, fn};

#ifdef ENABLE_CLI_EXTENDED_HELP
#define CLI_CMD_DEF_EXT(name, fn, synopsis, brief) \
  static const cli_cmd_t CLI_JOIN(cli, __LINE__) __attribute__ ((used, section("clicmd."#name))) = { name, fn, synopsis, brief};
#else
#define CLI_CMD_DEF_EXT(name, fn, synopsis, brief) \
  static const cli_cmd_t CLI_JOIN(cli, __LINE__) __attribute__ ((used, section("clicmd."#name))) = { name, fn};
#endif

#define cli_printf(cli, fmt...) stprintf((cli)->cl_stream, fmt)

int cli_getc(cli_t *cli, int wait);

int cli_on_stream(struct stream *s, char promptchar);

void cli_console(char promptchar);

#define cli_flush(cli) stream_write((cli)->cl_stream, NULL, 0, 0)
