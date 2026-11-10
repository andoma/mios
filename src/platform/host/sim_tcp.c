#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <mios/mios.h>

#include "sim.h"
#include "sim_tcp.h"

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

static inline uint16_t rd16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline uint32_t rd32(const uint8_t *p) { return ((uint32_t)rd16(p) << 16) | rd16(p + 2); }
static inline void wr32(uint8_t *p, uint32_t v) { wr16(p, v >> 16); wr16(p + 2, v); }
static inline uint32_t rd32n(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void wr32n(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint32_t
cksum_add(uint32_t sum, const uint8_t *p, size_t len)
{
  while(len > 1) {
    sum += rd16(p);
    p += 2;
    len -= 2;
  }
  if(len)
    sum += p[0] << 8;
  return sum;
}

static uint16_t
cksum_fold(uint32_t sum)
{
  while(sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return ~sum;
}


static void
sim_tcp_arp_reply(sim_tcp_t *t, const uint8_t *req)
{
  uint8_t f[42];
  memcpy(f, req + 6, 6);
  memcpy(f + 6, t->mac, 6);
  wr16(f + 12, ETHERTYPE_ARP);
  uint8_t *a = f + 14;
  wr16(a + 0, 1);               // Ethernet
  wr16(a + 2, ETHERTYPE_IPV4);
  a[4] = 6;
  a[5] = 4;
  wr16(a + 6, 2);               // reply
  memcpy(a + 8, t->mac, 6);
  wr32n(a + 14, t->ip);
  memcpy(a + 18, req + 14 + 8, 10); // target hw + proto addr = requester
  vnet_peer_send(t->vnet, f, sizeof(f));
}


static void
sim_tcp_send_raw(sim_tcp_t *t, int flags, uint32_t seq, uint32_t ack,
                 uint16_t wnd, const void *data, size_t len)
{
  uint8_t *f = t->txframe;

  memcpy(f, t->mios_mac, 6);
  memcpy(f + 6, t->mac, 6);
  wr16(f + 12, ETHERTYPE_IPV4);

  uint8_t *ip = f + 14;
  uint8_t *th = ip + 20;
  uint8_t *o = th + 20;

  if(flags & SIM_TCP_SYN) {
    if(t->mss) {
      *o++ = 2; *o++ = 4; wr16(o, t->mss); o += 2;
    }
    if(t->wscale >= 0) {
      *o++ = 1; *o++ = 3; *o++ = 3; *o++ = t->wscale;
    }
    if(t->sack_permitted) {
      *o++ = 1; *o++ = 1; *o++ = 4; *o++ = 2;
    }
  } else if(t->nsack) {
    *o++ = 1; *o++ = 1; *o++ = 5; *o++ = 2 + 8 * t->nsack;
    for(int i = 0; i < t->nsack; i++) {
      wr32(o, t->mios_isn + 1 + t->sack[i][0]);
      wr32(o + 4, t->mios_isn + 1 + t->sack[i][1]);
      o += 8;
    }
  }
  const size_t hdrlen = o - th;
  if(len)
    memcpy(o, data, len);
  const size_t tcplen = hdrlen + len;

  wr16(th + 0, t->port);
  wr16(th + 2, t->mios_port);
  wr32(th + 4, seq);
  wr32(th + 8, ack);
  th[12] = (hdrlen / 4) << 4;
  th[13] = flags;
  wr16(th + 14, wnd);
  wr16(th + 16, 0);
  wr16(th + 18, 0);

  memset(ip, 0, 20);
  ip[0] = 0x45;
  wr16(ip + 2, 20 + tcplen);
  wr16(ip + 4, t->ip_id++);
  ip[8] = 64;
  ip[9] = 6;
  wr32n(ip + 12, t->ip);
  wr32n(ip + 16, t->mios_ip);
  wr16(ip + 10, cksum_fold(cksum_add(0, ip, 20)));

  uint8_t pseudo[12];
  memcpy(pseudo, ip + 12, 8);
  pseudo[8] = 0;
  pseudo[9] = 6;
  wr16(pseudo + 10, tcplen);
  uint32_t sum = cksum_add(0, pseudo, 12);
  sum = cksum_add(sum, th, tcplen);
  wr16(th + 16, cksum_fold(sum));

  if(t->tx != NULL)
    t->tx(t, f, 14 + 20 + tcplen);
  else
    vnet_peer_send(t->vnet, f, 14 + 20 + tcplen);
}


void
sim_tcp_send(sim_tcp_t *t, int flags, uint32_t seq, uint32_t ack,
             uint16_t wnd, const void *data, size_t len)
{
  sim_tcp_send_raw(t, flags, t->isn + 1 + seq,
                   flags & SIM_TCP_ACK ? t->mios_isn + 1 + ack : 0,
                   wnd, data, len);
}


int
sim_tcp_input(sim_tcp_t *t, const uint8_t *f, size_t len, sim_tcp_seg_t *seg)
{
  if(len < 14)
    return 0;
  const uint16_t etype = rd16(f + 12);

  if(etype == ETHERTYPE_ARP && len >= 42) {
    const uint8_t *a = f + 14;
    if(rd16(a + 6) == 1 && rd32n(a + 24) == t->ip)
      sim_tcp_arp_reply(t, f);
    return 0;
  }

  if(etype != ETHERTYPE_IPV4 || len < 14 + 20 + 20)
    return 0;
  const uint8_t *ip = f + 14;
  if(ip[0] != 0x45 || ip[9] != 6 || rd32n(ip + 16) != t->ip)
    return 0;
  const size_t iplen = rd16(ip + 2);
  if(iplen < 40 || 14 + iplen > len)
    return 0;
  const uint8_t *th = ip + 20;
  if(rd16(th + 0) != t->mios_port || rd16(th + 2) != t->port)
    return 0;
  const size_t hdrlen = (th[12] >> 4) * 4;
  if(hdrlen < 20 || hdrlen > iplen - 20)
    return 0;

  memcpy(t->mios_mac, f + 6, 6);

  seg->at = clock_get();
  seg->flags = th[13];
  seg->wnd = rd16(th + 14);
  seg->len = iplen - 20 - hdrlen;
  seg->data = th + hdrlen;
  seg->mss = -1;
  seg->wscale = -1;
  seg->sack_permitted = 0;
  seg->nsack = 0;

  const uint8_t *o = th + 20;
  const uint8_t *end = th + hdrlen;
  while(o < end && *o) {
    if(*o == 1) {
      o++;
      continue;
    }
    if(o + 2 > end || o[1] < 2 || o + o[1] > end)
      break;
    if(o[0] == 2 && o[1] == 4)
      seg->mss = rd16(o + 2);
    if(o[0] == 3 && o[1] == 3)
      seg->wscale = o[2];
    if(o[0] == 4 && o[1] == 2)
      seg->sack_permitted = 1;
    if(o[0] == 5 && o[1] >= 10 && o[1] <= 34 && !((o[1] - 2) & 7)) {
      seg->nsack = (o[1] - 2) / 8;
      for(int i = 0; i < seg->nsack; i++) {
        seg->sack[i][0] = rd32(o + 2 + i * 8) - (t->isn + 1);
        seg->sack[i][1] = rd32(o + 6 + i * 8) - (t->isn + 1);
      }
    }
    o += o[1];
  }

  const uint32_t seq = rd32(th + 4);
  if(seg->flags & SIM_TCP_SYN) {
    t->mios_isn = seq;
    seg->seq = seq;
  } else {
    seg->seq = seq - (t->mios_isn + 1);
  }
  seg->ack = seg->flags & SIM_TCP_ACK ? rd32(th + 8) - (t->isn + 1) : 0;
  t->rx_segments++;
  return 1;
}


int
sim_tcp_recv(sim_tcp_t *t, sim_tcp_seg_t *seg, uint64_t timeout)
{
  const uint64_t deadline = clock_get() + timeout;
  while(1) {
    const ssize_t n = vnet_peer_recv(t->vnet, t->rxframe, sizeof(t->rxframe),
                                     deadline);
    if(n < 0)
      return 0;
    if(sim_tcp_input(t, t->rxframe, n, seg))
      return 1;
  }
}


int
sim_tcp_connect(sim_tcp_t *t, uint16_t wnd, sim_tcp_seg_t *synack)
{
  sim_tcp_seg_t seg;
  if(synack == NULL)
    synack = &seg;

  // Mios's address is known but its MAC is not; broadcast the SYN
  memset(t->mios_mac, 0xff, 6);
  sim_tcp_send_raw(t, SIM_TCP_SYN, t->isn, 0, wnd, NULL, 0);
  if(!sim_tcp_recv(t, synack, 1000000))
    return 0;
  if(synack->flags != (SIM_TCP_SYN | SIM_TCP_ACK) || synack->ack != 0)
    return 0;
  sim_tcp_send(t, SIM_TCP_ACK, 0, 0, wnd, NULL, 0);
  return 1;
}


int
sim_tcp_accept_syn(sim_tcp_t *t, uint16_t wnd, sim_tcp_seg_t *syn,
                   uint64_t timeout)
{
  sim_tcp_seg_t seg;
  if(syn == NULL)
    syn = &seg;

  // The port Mios connects from is its to pick: take it from the SYN
  t->mios_port = 0;
  const uint64_t deadline = clock_get() + timeout;
  while(1) {
    const ssize_t n = vnet_peer_recv(t->vnet, t->rxframe, sizeof(t->rxframe),
                                     deadline);
    if(n < 0)
      return 0;
    const uint8_t *f = t->rxframe;
    if(n >= 54 && rd16(f + 12) == ETHERTYPE_IPV4 && f[14 + 9] == 6 &&
       rd16(f + 34 + 2) == t->port && f[34 + 13] == SIM_TCP_SYN)
      t->mios_port = rd16(f + 34);
    if(sim_tcp_input(t, f, n, syn) && syn->flags == SIM_TCP_SYN)
      break;
  }

  sim_tcp_send_raw(t, SIM_TCP_SYN | SIM_TCP_ACK, t->isn, t->mios_isn + 1,
                   wnd, NULL, 0);
  return 1;
}


int
sim_tcp_accept(sim_tcp_t *t, uint16_t wnd, sim_tcp_seg_t *syn,
               uint64_t timeout)
{
  sim_tcp_seg_t seg;
  if(!sim_tcp_accept_syn(t, wnd, syn, timeout))
    return 0;
  if(!sim_tcp_recv(t, &seg, 1000000))
    return 0;
  return seg.flags == SIM_TCP_ACK && seg.seq == 0 && seg.ack == 0;
}
