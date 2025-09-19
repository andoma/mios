#include "t234ccplex_bpmp.h"
#include "t234ccplex_clk.h"

#include <stdio.h>
#include <string.h>

#include <mios/mios.h>
#include <mios/cli.h>

#include "irq.h"
#include "reg.h"

#include <unistd.h>


#define XUSB_PADCTL_BASE 0x3520000

#define XUSB_PADCTL_USB2_PORT_CAP_0                     0x8

static void __attribute__((constructor(300)))
xusb_init(void)
{
  // Turn off USB2 ports. This makes us disconnect from host in RCMBOOT
  reg_wr(XUSB_PADCTL_BASE + XUSB_PADCTL_USB2_PORT_CAP_0, 0);
}


static error_t
cmd_xusb(cli_t *cli, int argc, char **argv)
{
  error_t err;

  if((err = bpmp_powergate_set(10, 1)) != 0)
    return err;
  if((err = bpmp_powergate_set(12, 1)) != 0)
    return err;
  cli_printf(cli, "xusb powered on\n");
  return 0;
}

CLI_CMD_DEF("xusb", cmd_xusb);
