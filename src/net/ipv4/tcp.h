#pragma once

#include <stddef.h>
#include <stdint.h>

struct netif;
struct pbuf;
struct strean;

struct pbuf *tcp_input_ipv4(struct netif *ni, struct pbuf *pb,
                            int udp_offset);

struct stream *tcp_create_socket(const char *name,
                                 size_t txfifo_size, size_t rxfifo_size);

void tcp_connect(struct stream *s, uint32_t dst_addr, uint16_t dst_port);
