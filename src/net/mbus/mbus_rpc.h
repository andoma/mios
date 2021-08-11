#pragma once

#include <stdint.h>

struct pbuf;
struct mbus_netif;

struct pbuf *mbus_handle_rpc_resolve(struct mbus_netif *mni, struct pbuf *pb,
                                     uint8_t remote_addr);

struct pbuf *mbus_handle_rpc_invoke(struct mbus_netif *mni, struct pbuf *pb,
                                    uint8_t remote_addr);
