#include <mios/device.h>
#include <mios/cli.h>

static STAILQ_HEAD(, device) devices = STAILQ_HEAD_INITIALIZER(devices);


void
device_register(device_t *d)
{
  STAILQ_INSERT_TAIL(&devices, d, d_link);
}


error_t
device_not_implemented(device_t *d)
{
  return ERR_NOT_IMPLEMENTED;
}


static int
cmd_dev(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "Devices:\n");

  device_t *d;
  STAILQ_FOREACH(d, &devices, d_link) {
    cli_printf(cli, "%s\n", d->d_name);
    if(d->d_print_info)
      d->d_print_info(cli->cl_stream);
  }
  return 0;
}

CLI_CMD_DEF("dev", cmd_dev);
