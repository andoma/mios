#pragma once

#include <stdint.h>

struct netif;
struct pbuf;
struct pushpull;

struct pbuf *tcp_input_ipv4(struct netif *ni, struct pbuf *pb,
                            int udp_offset);

struct pushpull *tcp_create_socket(const char *name);

void tcp_connect(struct pushpull *sk, uint32_t dst_addr, uint16_t dst_port);
