#pragma once

#include <stdint.h>

void igmp_group_join(uint32_t group_addr);
void igmp_group_leave(uint32_t group_addr);

struct pbuf *igmp_input_ipv4(struct netif *ni, struct pbuf *pb, int udp_offset);
