#include <stdlib.h>
#include <unistd.h>

#include <mios/cli.h>
#include <mios/flash.h>
#include <mios/mios.h>
#include <mios/pkv.h>
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

  const int start = atoix(argv[1]);
  const int len   = atoix(argv[2]);
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

  const int addr = atoix(argv[1]);
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

  const int start = atoix(argv[1]);
  const int count = argc > 2 ? atoix(argv[2]) : 1;

  uint32_t *ptr = (uint32_t *)start;
  for(int i = 0; i < count; i++) {
    cli_printf(cli, "0x%04x: 0x%08x\n",
           4 * i, *ptr);
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


static error_t
cmd_flash_info(cli_t *cli, int argc, char **argv)
{
  const flash_iface_t *fi = flash_get_primary();
  if(fi == NULL)
    return ERR_NO_DEVICE;

  for(int i = 0; ; i++) {
    size_t size = fi->get_sector_size(fi, i);
    if(size == 0)
      break;
    cli_printf(cli,
               "Sector: %2d  size: %7d  type: %d\n",
               i, size, fi->get_sector_type(fi, i));
  }
  return 0;
}



CLI_CMD_DEF("flash_info", cmd_flash_info);


static error_t
cmd_settings(cli_t *cli, int argc, char **argv)
{
  pkv_show(NULL, cli->cl_stream);
  return 0;
}

CLI_CMD_DEF("settings", cmd_settings)


static error_t
cmd_version(cli_t *cli, int argc, char **argv)
{
  mios_print_version(cli->cl_stream);
  return 0;
}

CLI_CMD_DEF("version", cmd_version)




static const char *reset_reasons[8] = {
  [0] = "Unknown",
  [RESET_REASON_LOW_POWER_RESET] = "Low power",
  [RESET_REASON_WATCHDOG] = "Watchdog",
  [RESET_REASON_SW_RESET] = "SW-Reset",
  [RESET_REASON_POWER_ON] = "Power On",
  [RESET_REASON_EXT_RESET] = "External",
  [RESET_REASON_BROWNOUT] = "Brownout",
};



static error_t
cmd_sysinfo(cli_t *cli, int argc, char **argv)
{
  const struct serial_number sn = sys_get_serial_number();

  if(sn.len) {
    cli_printf(cli, "Serial number: ");
    const uint8_t *d8 = sn.data;
    for(int i = 0; i < 12 ; i++) {
      cli_printf(cli, "%02x", d8[i]);
    }
    cli_printf(cli, "\n");
  }
  cli_printf(cli, "Last reset reason: %s\n",
             reset_reasons[sys_get_reset_reason()]);
  return 0;
}

CLI_CMD_DEF("sysinfo", cmd_sysinfo);
