#pragma once

#include "netif.h"

pbuf_t *dsig_udp_output(struct netif *ni, pbuf_t *pb, uint32_t id,
                        uint32_t flags);
