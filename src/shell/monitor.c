#include <stdlib.h>
#include <unistd.h>

#include <mios/cli.h>
#include <mios/flash.h>
#include <mios/mios.h>
#include <mios/pkv.h>
#include <mios/version.h>


static int
cmd_reset(cli_t *cli, int argc, char **argv)
{
  reboot();
  return 0;
}

CLI_CMD_DEF("reset", cmd_reset);


static int
cmd_md(cli_t *cli, int argc, char **argv)
{
  if(argc < 3) {
    cli_printf(cli, "md <start> <length>\n");
    return -1;
  }

  const int start = atoix(argv[1]);
  const int len   = atoix(argv[2]);
  sthexdump(cli->cl_stream, NULL, (void *)start, len, start);
  return 0;
}



CLI_CMD_DEF("md", cmd_md);




static int
cmd_rd32(cli_t *cli, int argc, char **argv)
{
  if(argc < 3) {
    cli_printf(cli, "rd32 <start> <count>\n");
    return -1;
  }

  const int start = atoix(argv[1]);
  const int count = atoix(argv[2]);

  uint32_t *ptr = (uint32_t *)start;
  for(int i = 0; i < count; i++) {
    cli_printf(cli, "0x%04x: 0x%08x\n",
           4 * i, *ptr);
    ptr++;
  }
  return 0;
}



CLI_CMD_DEF("rd32", cmd_rd32);



static int
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


static int
cmd_flash_info(cli_t *cli, int argc, char **argv)
{
  const flash_iface_t *fi = flash_get_primary();
  if(fi == NULL)
    return 0;

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


static int
cmd_settings(cli_t *cli, int argc, char **argv)
{
  pkv_show(NULL, cli->cl_stream);
  return 0;
}

CLI_CMD_DEF("settings", cmd_settings)


static int
cmd_version(cli_t *cli, int argc, char **argv)
{
  mios_print_version(cli->cl_stream);
  return 0;
}

CLI_CMD_DEF("version", cmd_version)
