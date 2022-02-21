#include <mios/device.h>
#include <mios/cli.h>

static STAILQ_HEAD(, device) devices = STAILQ_HEAD_INITIALIZER(devices);


void
device_register(device_t *d)
{
  STAILQ_INSERT_TAIL(&devices, d, d_link);
}

static error_t
cmd_dev(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "\nDevices:\n");

  device_t *d;
  STAILQ_FOREACH(d, &devices, d_link) {
    cli_printf(cli, "\n[%s]\n", d->d_name);
    if(d->d_class->dc_print_info)
      d->d_class->dc_print_info(d, cli->cl_stream);
  }
  cli_printf(cli, "\n");
  return 0;
}

CLI_CMD_DEF("dev", cmd_dev);


void
device_power_state(device_power_state_t state)
{
  device_t *d;
  STAILQ_FOREACH(d, &devices, d_link) {
    if(d->d_class->dc_power_state)
      d->d_class->dc_power_state(d, state);
  }
}
