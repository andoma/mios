#include "ble.h"

#include <mios/cli.h>
#include <mios/service.h>
#include <mios/stream.h>

void
ble_print_connection_header(struct stream *st, int idx,
                            const uint8_t *addr, const char *state,
                            int sec_level, int interval_us,
                            int timeout_us)
{
  static const char *secstr[4] = {
    "plaintext", "encrypted", "authenticated", "authenticated (SC)"
  };
  stprintf(st, "conn%d: %02x:%02x:%02x:%02x:%02x:%02x  %s  %s\n",
           idx, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
           state, secstr[sec_level & 3]);
  stprintf(st, "  interval: %d.%02dms  timeout: %dms\n",
           interval_us / 1000, (interval_us % 1000) / 10,
           timeout_us / 1000);
}

// Each BLE stack provides the real one
__attribute__((weak)) void
ble_print_connections(struct stream *st)
{
  stprintf(st, "No BLE stack\n");
}

static error_t
cmd_connections(cli_t *cli, int argc, char **argv)
{
  ble_print_connections(cli->cl_stream);
  return 0;
}

CLI_CMD_DEF_EXT("ble_connections", cmd_connections, "",
                "Show active BLE connections");
