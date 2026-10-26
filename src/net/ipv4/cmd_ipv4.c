#include <mios/cli.h>
#include <string.h>

#include "net/netif.h"
#include "net/net.h"
#include "ipv4.h"

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


CLI_CMD_DEF_EXT("show_arp", cmd_arp, NULL, "Show ARP table");


static error_t
cmd_route(cli_t *cli, int argc, char **argv)
{
  const netif_t *def = ipv4_default_route();
  netif_t *ni = NULL;

  while((ni = netif_get_net(ni)) != NULL) {
    if(ni->ni_ipv4_local_addr == 0)
      continue;
    const uint32_t mask =
      htonl(mask_from_prefixlen(ni->ni_ipv4_local_prefixlen));
    cli_printf(cli, "%Id/%d dev %s\n", ni->ni_ipv4_local_addr & mask,
               ni->ni_ipv4_local_prefixlen, ni->ni_dev.d_name);
  }

  while((ni = netif_get_net(ni)) != NULL) {
    if(ni->ni_ipv4_gateway == 0)
      continue;
    cli_printf(cli, "default via %Id dev %s metric %d%s%s\n",
               ni->ni_ipv4_gateway, ni->ni_dev.d_name,
               ni->ni_ipv4_route_metric,
               ni->ni_flags & NETIF_F_UP ? "" : " (link down)",
               ni == def ? " (active)" : "");
  }
  return 0;
}

CLI_CMD_DEF_EXT("show_route", cmd_route, NULL, "Show IPv4 routes");
