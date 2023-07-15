#pragma once

#include <stdint.h>

struct pbuf;

struct pbuf *mbus_seqpkt_accept(struct pbuf *pb, uint8_t src_addr,
                                uint16_t flow);
