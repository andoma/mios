#pragma once

#include <stdint.h>

struct pbuf;

struct pbuf *mbus_seqpkt_input(struct pbuf *pb, uint8_t src_addr);
