#pragma once

#include "ipv4.h"

struct ether_netif;
struct netif;
struct pbuf;
struct stream;

void dhcpv4_status_change(struct ether_netif *eni);

struct pbuf *dhcpv4_input(struct netif *ni, struct pbuf *pb,
                          uint32_t src_addr);

void dhcpv4_print(struct ether_netif *eni, struct stream *st);
