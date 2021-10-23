#pragma once

#include <stdint.h>

struct pbuf;
struct mbus_netif;

struct pbuf *mbus_dsig_input(struct mbus_netif *mni, struct pbuf *pb,
                             uint8_t remote_addr);
