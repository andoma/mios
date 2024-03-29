#include <mios/cli.h>
#include <string.h>

#include "net/netif.h"

static error_t
cmd_arp(cli_t *cli, int argc, char **argv)
{
  netif_t *ni = NULL;

  while((ni = netif_get_net(ni)) != NULL) {
    const nexthop_t *nh;

    LIST_FOREACH(nh, &ni->ni_nexthops, nh_netif_link) {
      cli_printf(cli, "%Id\t", nh->nh_addr);
      if(nh->nh_state == 0) {
        cli_printf(cli, "<idle>\n");
      } else if(nh->nh_state <= NEXTHOP_RESOLVE) {
        cli_printf(cli, "<resolve>\n");
      } else {
        cli_printf(cli, "%02x%02x.%02x%02x.%02x%02x %ds%s\n",
                   nh->nh_hwaddr[0],
                   nh->nh_hwaddr[1],
                   nh->nh_hwaddr[2],
                   nh->nh_hwaddr[3],
                   nh->nh_hwaddr[4],
                   nh->nh_hwaddr[5],
                   nh->nh_state,
                   nh->nh_in_use ? " (Active)" : "");
      }
    }
  }

  return 0;
}


CLI_CMD_DEF("arp", cmd_arp);
