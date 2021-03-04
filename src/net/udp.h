#pragma once

#include <mios/error.h>

struct netif;
struct pbuf;
struct socket;

struct pbuf *udp_input_ipv4(struct netif *ni, struct pbuf *pb,
                            int udp_offset);

error_t udp_socket_attach(struct socket *s);

error_t udp_socket_detach(struct socket *s);
