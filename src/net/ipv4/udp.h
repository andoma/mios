#pragma once

struct netif;
struct pbuf;
struct socket;

struct pbuf *udp_input_ipv4(struct netif *ni, struct pbuf *pb,
                            int udp_offset);
