#include "udp.h"

#include <sys/param.h>
#include <string.h>
#include <stdio.h>

#include "net/pbuf.h"
#include "net/ether.h"
#include "net/ipv4/ipv4.h"
#include "net/ipv4/udp.h"
#include "net/net.h"
#include "net/net_task.h"

#include <mios/bytestream.h>
#include <mios/hostname.h>

#define INADDR_MDNS htonl(0xe00000fb) // 224.0.0.251

typedef struct {
  uint16_t transaction_id;
  uint16_t flags;
  uint16_t questions;
  uint16_t answer_rr;
  uint16_t authority_rr;
  uint16_t additional_rr;
} dns_header_t;

static int
dns_is_name(pbuf_t *pb, size_t offset, int *yes,
            const char **names, size_t veclen)
{
  int used = 0;
  uint8_t labellen;

  *yes = 1;

  while(1) {
    if(pbuf_read_at(pb, &labellen, offset + used, 1))
      return -1;
    if(labellen >= 0xc0) {
      break;
    }
    if(labellen >= 0x40) {
      return -1;
    }
    used++;
    if(labellen == 0) {
      if(veclen != 0)
        *yes = 0;
      return used;
    }

    if(veclen) {
      int len = strlen(*names);
      if(labellen != len ||
         pbuf_memcmp_at(pb, *names, offset + used, labellen)) {
        *yes = 0;
      }
      names++;
      veclen--;
    } else {
      *yes = 0;
    }
    used += labellen;
  }

  offset += used;
  used += 2;

  int prev_offset = offset;
  while(1) {
    uint16_t u16;
    if(pbuf_read_at(pb, &u16, offset, sizeof(uint16_t)))
      return -1;
    u16 = ntohs(u16) & 0x3fff;
    if(u16 >= prev_offset)
      return -1;
    offset = u16;
    prev_offset = offset;
    while(1) {
      if(pbuf_read_at(pb, &labellen, offset, 1))
        return -1;
      if(labellen >= 0xc0) {
        break;
      }
      if(labellen >= 0x40) {
        return -1;
      }
      offset++;
      if(labellen == 0) {
        if(veclen)
          *yes = 0;
        return used;
      }

      if(veclen) {
        int len = strlen(*names);
        if(labellen != len ||
           pbuf_memcmp_at(pb, *names, offset, labellen)) {
          *yes = 0;
        }
        names++;
        veclen--;
      }
      offset += labellen;
    }
  }
  return used;
}

static void
mdns_send_response(struct netif *ni, uint32_t from, uint16_t transaction_id,
                   const char *dnsname, int send_unicast, int type)
{
  pbuf_t *pb = pbuf_make(16 + 20 + 8, 0);
  if(pb == NULL)
    return;

  dns_header_t *dh = pbuf_append(pb, sizeof(dns_header_t));
  dh->transaction_id = transaction_id;
  dh->flags = htons(0x8400);
  dh->questions = 0;
  dh->answer_rr = htons(1);
  dh->authority_rr = 0;
  dh->additional_rr = 0;

  int dnsname_len = strlen(dnsname);
  uint8_t *name = pbuf_append(pb, 1 + dnsname_len + 7);
  name[0] = dnsname_len;
  memcpy(name + 1, dnsname, dnsname_len);
  memcpy(name + 1 + dnsname_len, "\x05local", 7);

  if(type == 1) {
    uint8_t *rr = pbuf_append(pb, 2 + 2 + 4 + 2 + 4);
    wr16_be(rr + 0, 1);      // Type: A
    wr16_be(rr + 2, 0x8001); // Class: IN + CacheFlush
    wr32_be(rr + 4, 60);     // TTL
    wr16_be(rr + 8, 4);      // IPv4 length
    memcpy(rr + 10, &ni->ni_local_addr, 4);
  } else {
    uint8_t *rr = pbuf_append(pb, 2 + 2 + 4 + 2 + 2 + 3);
    wr16_be(rr + 0, 47);      // Type: NSEC
    wr16_be(rr + 2, 0x8001);  // Class: IN + CacheFlush
    wr32_be(rr + 4, 60);      // TTL
    wr16_be(rr + 8, 5);       // NSEC length
    wr16_be(rr + 10, 0xc00c); // Next authoritive (point to itself)
    rr[12] = 0;               // Window block #
    rr[13] = 1;               // Block length
    rr[14]  = 0x40;           // Type A
  }

  if(send_unicast) {
    udp_send(NULL, pb, from, NULL, 5353, 5353);
  } else {
    udp_send(ni, pb, INADDR_MDNS, NULL, 5353, 5353);
  }
}

static pbuf_t *
mdns_input_locked(struct netif *ni, pbuf_t *pb, size_t udp_offset)
{
  const ipv4_header_t *ip = pbuf_data(pb, 0);
  const uint32_t from = ip->src_addr;

  pb = pbuf_drop(pb, udp_offset + 8);

  if(pbuf_pullup(pb, sizeof(dns_header_t)))
    return pb;
  const dns_header_t *dh = (const dns_header_t *)pbuf_cdata(pb, 0);
  uint16_t questions = htons(dh->questions);

  const char *our_name[] = {hostname, "local"};

  size_t offset = sizeof(dns_header_t);
  for(; questions > 0; questions--) {

    int for_us = 0;
    int r = dns_is_name(pb, offset, &for_us, our_name, 2);
    if(r < 0)
      return pb; // Damanged packet

    offset += r;
    uint16_t type;
    uint16_t class;
    if(pbuf_read_at(pb, &type, offset, sizeof(uint16_t)))
      return pb;
    if(pbuf_read_at(pb, &class, offset + 2, sizeof(uint16_t)))
      return pb;
    offset += 4;

    if(!for_us)
      continue;

    type = ntohs(type);
    class = ntohs(class);

    if((class & 0x7fff) != 1)
      continue;

    int send_unicast = class & 0x8000;

    mdns_send_response(ni, from, dh->transaction_id,
                       hostname, send_unicast, type);
  }
  return pb;
}

static pbuf_t *
mdns_input(struct netif *ni, pbuf_t *pb, size_t udp_offset)
{
  mutex_lock(&hostname_mutex);
  if(hostname[0]) {
    pb = mdns_input_locked(ni, pb, udp_offset);
  }
  mutex_unlock(&hostname_mutex);
  return pb;
}

UDP_INPUT(mdns_input, 5353);
