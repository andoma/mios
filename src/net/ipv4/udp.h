#pragma once

#include <stddef.h>
#include <stdint.h>
#include <mios/mios.h>

struct netif;
struct pbuf;
struct nexthop;

struct pbuf *udp_input_ipv4(struct netif *ni, struct pbuf *pb,
                            size_t udp_offset);

void udp_send(struct netif *ni, struct pbuf *pb, uint32_t dst_addr,
              struct nexthop *nh, int src_port, int dst_port);

typedef struct {
  struct pbuf *(*input)(struct netif *ni, struct pbuf *pb, size_t udp_offset);
  uint16_t port;
} udp_input_t;

#define UDP_INPUT(cb, port)                                                  \
  static const udp_input_t MIOS_JOIN(udpinput, __LINE__) __attribute__ ((used, section("udpinput"))) = { cb, port };
