#pragma once

#include <stdint.h>

struct pbuf;
struct netif;

struct pbuf *dsig_input(uint32_t id, struct pbuf *pb, struct netif *ni);
