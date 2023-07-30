#include <mios/device.h>
#include <mios/cli.h>
#include <mios/task.h>

#include "irq.h"

static STAILQ_HEAD(, device) devices = STAILQ_HEAD_INITIALIZER(devices);
static mutex_t devs_mutex = MUTEX_INITIALIZER("devss");


void
device_register(device_t *d)
{
  d->d_refcount = 1;

  mutex_lock(&devs_mutex);
  STAILQ_INSERT_TAIL(&devices, d, d_link);
  mutex_unlock(&devs_mutex);
}

void
device_unregister(device_t *d)
{
  mutex_lock(&devs_mutex);
  STAILQ_REMOVE(&devices, d, device, d_link);
  mutex_unlock(&devs_mutex);
  device_release(d);
}


void
device_release(device_t *d)
{
  if(__sync_add_and_fetch(&d->d_refcount, -1))
    return;

  if(d->d_class->dc_dtor == NULL)
    panic("Release device %s with no dtor", d->d_name);
  d->d_class->dc_dtor(d);
}

void
device_retain(device_t *d)
{
  __sync_add_and_fetch(&d->d_refcount, 1);
}

device_t *
device_get_next(device_t *cur)
{
  device_t *d;
  mutex_lock(&devs_mutex);

  if(cur == NULL) {
    d = STAILQ_FIRST(&devices);
  } else {
    STAILQ_FOREACH(d, &devices, d_link) {
      if(d == cur) {
        break;
      }
    }
    if(d)
      d = STAILQ_NEXT(d, d_link);
  }

  if(d)
    device_retain(d);
  mutex_unlock(&devs_mutex);
  if(cur)
    device_release(cur);
  return d;
}


static error_t
cmd_dev(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "\nDevices:\n");

  device_t *d = NULL;
  while((d = device_get_next(d)) != NULL) {
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
