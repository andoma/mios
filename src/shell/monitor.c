#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <mios/cli.h>
#include <mios/mios.h>
#include <mios/version.h>
#include <mios/sys.h>


static error_t
cmd_reset(cli_t *cli, int argc, char **argv)
{
  reboot();
  return 0;
}

CLI_CMD_DEF("reset", cmd_reset);


static error_t
cmd_md(cli_t *cli, int argc, char **argv)
{
  if(argc < 3) {
    cli_printf(cli, "md <start> <length>\n");
    return ERR_INVALID_ARGS;
  }

  const long start = atolx(argv[1]);
  const long len   = atolx(argv[2]);
  sthexdump(cli->cl_stream, NULL, (void *)start, len, start);
  return 0;
}



CLI_CMD_DEF("md", cmd_md);


static error_t
cmd_wr32(cli_t *cli, int argc, char **argv)
{
  if(argc < 3) {
    cli_printf(cli, "wr32 <addr> <value>\n");
    return ERR_INVALID_ARGS;
  }

  const long addr = atoix(argv[1]);
  const int value = atoix(argv[2]);

  uint32_t *ptr = (uint32_t *)addr;
  *ptr = value;
  return 0;
}



CLI_CMD_DEF("wr32", cmd_wr32);



static error_t
cmd_rd32(cli_t *cli, int argc, char **argv)
{
  if(argc < 2) {
    cli_printf(cli, "rd32 <start> [count]\n");
    return ERR_INVALID_ARGS;
  }

  const long start = atoix(argv[1]);
  const int count = argc > 2 ? atoix(argv[2]) : 1;

  uint32_t *ptr = (uint32_t *)start;
  for(int i = 0; i < count; i++) {
    uint32_t val = *ptr;
    cli_printf(cli, "0x%04x: 0x%08x ",
           4 * i, val);

    for(int i = 28; i >= 0; i -= 4) {
      cli_printf(cli, "%c%c%c%c%s",
                 ((val >> (i + 3)) & 1) + '0',
                 ((val >> (i + 2)) & 1) + '0',
                 ((val >> (i + 1)) & 1) + '0',
                 ((val >> (i + 0)) & 1) + '0',
                 i ? "_" : "\n");
    }
    ptr++;
  }
  return 0;
}



CLI_CMD_DEF("rd32", cmd_rd32);



static error_t
cmd_uptime(cli_t *cli, int argc, char **argv)
{
  int64_t now = clock_get();

  int seconds = now / 1000000LLU;

  int h = seconds / 3600;
  int m = (seconds / 60) % 60;
  int s = seconds % 60;

  cli_printf(cli, "%02d:%02d:%02d (%d seconds)\n",
             h, m, s, seconds);
  return 0;
}



CLI_CMD_DEF("uptime", cmd_uptime);


const char *reset_reasons =
  "Low-Power\0"
  "Watchdog\0"
  "Software\0"
  "Power-On\0"
  "External\0"
  "Brownout\0"
  "CPU-lockup\0"
  "GPIO\0"
  "Comp\0"
  "NFC\0";

static error_t
cmd_sysinfo(cli_t *cli, int argc, char **argv)
{
  const struct serial_number sn = sys_get_serial_number();

  mios_print_version(cli->cl_stream);

  if(sn.len) {
    cli_printf(cli, "Serial number: ");
    const uint8_t *d8 = sn.data;
    for(int i = 0; i < sn.len ; i++) {
      cli_printf(cli, "%02x", d8[i]);
    }
    cli_printf(cli, "\n");
  }
  uint32_t rr = sys_get_reset_reason();
  if(rr) {
    cli_printf(cli, "Last reset reason: ");
    stprintflags(cli->cl_stream, reset_reasons, rr, " ");
    cli_printf(cli, "\n");
  }
  return 0;
}

CLI_CMD_DEF("sysinfo", cmd_sysinfo);
