#include <mios/cli.h>
#include <string.h>

#include "netif.h"

static int
cmd_arp(cli_t *cli, int argc, char **argv)
{
  const netif_t *ni;

  mutex_lock(&netif_mutex);

  SLIST_FOREACH(ni, &netifs, ni_global_link) {
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
  mutex_unlock(&netif_mutex);

  return 0;
}


CLI_CMD_DEF("arp", cmd_arp);
