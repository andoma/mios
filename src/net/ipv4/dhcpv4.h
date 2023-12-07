#pragma once

#include "ipv4.h"

struct ether_netif;
struct netif;
struct pbuf;
struct stream;

void dhcpv4_status_change(struct ether_netif *eni);

void dhcpv4_print(struct ether_netif *eni, struct stream *st);

typedef int(*dhcpv4_update_t)(struct netif *ni);

#define DHCPV4UPDATE(cb)                                                  \
  static const dhcpv4_update_t MIOS_JOIN(dhcpv4update, __LINE__) __attribute__ ((used, section("dhcpv4update"))) = cb;
