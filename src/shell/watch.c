#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#include <mios/cli.h>
#include <mios/mios.h>


static error_t
cmd_watch(cli_t *cli, int argc, char **argv)
{
  const uint64_t watchtime = 1000000;
  uint64_t deadline = clock_get() + watchtime;
  if (argc == 1)  {
    cli_printf(cli, "watch <command> <args>\n");
    return ERR_INVALID_ARGS;
  }
  uint8_t c = 0;
  const pollset_t ps = {
    .obj = cli->cl_stream,
    .type = POLL_STREAM_READ,
  };
  while (c != 0x03) {
    error_t err = cli_dispatch_command(cli, argc - 1, argv + 1);
    if (err)
      return err;

    if (poll(&ps, 1, NULL, deadline) < 0) {
      deadline += watchtime;
      continue;
    }
    stream_read(cli->cl_stream, &c, 1, 1);
  }
  return ERR_OK;
}

CLI_CMD_DEF("watch", cmd_watch);
