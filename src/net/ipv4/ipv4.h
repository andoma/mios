#pragma once

#include <stdint.h>

struct pbuf;
struct netif;
struct nexthop;

typedef struct ipv4_header {

  uint8_t ver_ihl;
  uint8_t tos;
  uint16_t total_length;
  uint16_t id;
  uint16_t fragment_info;
  uint8_t ttl;
  uint8_t proto;
  uint16_t cksum;
  uint32_t src_addr;
  uint32_t dst_addr;

} ipv4_header_t;

#define IPV4_F_DF 0x4000
#define IPV4_F_MF 0x2000
#define IPV4_F_FO 0x1fff


typedef struct icmp_hdr {
  uint8_t type;
  uint8_t code;
  uint16_t cksum;
} icmp_hdr_t;


typedef struct udp_hdr {
  uint16_t src_port;
  uint16_t dst_port;
  uint16_t length;
  uint16_t cksum;
} udp_hdr_t;


typedef struct tcp_hdr {
  uint16_t src_port;
  uint16_t dst_port;
  uint32_t seq;
  uint32_t ack;
  uint8_t off;
  uint8_t flg;
  uint16_t wnd;
  uint16_t cksum;
  uint16_t up;
} tcp_hdr_t;

#define TCP_F_FIN 0x01
#define TCP_F_SYN 0x02
#define TCP_F_RST 0x04
#define TCP_F_PSH 0x08
#define TCP_F_ACK 0x10
#define TCP_F_URG 0x20



#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

struct pbuf *ipv4_input(struct netif *ni, struct pbuf *pb);

uint32_t ipv4_cksum_pseudo(uint32_t src_addr, uint32_t dst_addr,
                           uint8_t protocol, uint16_t length);

uint16_t ipv4_cksum_pbuf(uint32_t sum, struct pbuf *pb, int offset, int length);

struct nexthop *ipv4_nexthop_resolve(uint32_t addr);
