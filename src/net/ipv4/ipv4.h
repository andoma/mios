#pragma once

struct pbuf;
struct netif;

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


#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

struct pbuf *ipv4_input(struct netif *ni, struct pbuf *pb);

void ipv4_output(pbuf_t *pb);

uint32_t ipv4_cksum_pseudo(uint32_t src_addr, uint32_t dst_addr,
                           uint8_t protocol, uint16_t length);

uint16_t ipv4_cksum_pbuf(uint32_t sum, struct pbuf *pb, int offset, int length);
