#pragma once

#include <stddef.h>
#include <stdint.h>

struct pbuf;

struct pbuf *mbus_rpc_dispatch(struct pbuf *pb, uint8_t src_addr,
                               uint16_t flow);
