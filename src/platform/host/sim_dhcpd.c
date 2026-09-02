#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <mios/mios.h>

#include "sim.h"
#include "sim_dhcpd.h"

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

static inline uint16_t rd16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
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


// ---- Transmit ----

static void
sim_dhcpd_send(sim_dhcpd_t *d, const uint8_t *frame, size_t len)
{
  if(d->reply_delay == 0) {
    vnet_peer_send(d->vnet, frame, len);
    return;
  }
  // One pending reply is enough for the scenarios we model; the
  // thread's receive loop delivers it when pending_at is reached.
  memcpy(d->pending, frame, len);
  d->pending_len = len;
  d->pending_at = clock_get() + d->reply_delay;
}


static void
sim_dhcpd_reply(sim_dhcpd_t *d, int msgtype, const uint8_t *client_mac,
                uint32_t xid, uint32_t yiaddr)
{
  uint8_t f[600];
  memset(f, 0, sizeof(f));

  // Ethernet
  memcpy(f, client_mac, 6);
  memcpy(f + 6, d->mac, 6);
  wr16(f + 12, ETHERTYPE_IPV4);

  // DHCP payload at 14 + 20 + 8
  uint8_t *dh = f + 42;
  dh[0] = 2;   // BOOTREPLY
  dh[1] = 1;   // Ethernet
  dh[2] = 6;
  wr32n(dh + 4, xid);
  wr32n(dh + 16, yiaddr);       // yiaddr
  wr32n(dh + 20, d->server_ip); // siaddr
  memcpy(dh + 28, client_mac, 6);
  dh[236] = 0x63; dh[237] = 0x82; dh[238] = 0x53; dh[239] = 0x63;

  uint8_t *o = dh + 240;
  *o++ = 53; *o++ = 1; *o++ = msgtype;
  *o++ = 54; *o++ = 4; wr32n(o, d->server_ip); o += 4;
  if(msgtype != DHCPNAK) {
    *o++ = 51; *o++ = 4; wr32n(o, __builtin_bswap32(d->lease_time)); o += 4;
    *o++ = 1;  *o++ = 4; wr32n(o, d->netmask); o += 4;
    *o++ = 3;  *o++ = 4; wr32n(o, d->gateway); o += 4;
    if(d->ntp_server) {
      *o++ = 42; *o++ = 4; wr32n(o, d->ntp_server); o += 4;
    }
  }
  *o++ = 255;

  // Pad BOOTP to the classic 300 byte minimum
  size_t dhcp_len = o - dh;
  if(dhcp_len < 300)
    dhcp_len = 300;

  // UDP 67 -> 68
  uint8_t *udp = f + 34;
  wr16(udp + 0, 67);
  wr16(udp + 2, 68);
  wr16(udp + 4, 8 + dhcp_len);
  wr16(udp + 6, 0);

  // IPv4
  uint8_t *ip = f + 14;
  const uint32_t dst = msgtype == DHCPNAK ? 0xffffffff : yiaddr;
  ip[0] = 0x45;
  wr16(ip + 2, 20 + 8 + dhcp_len);
  wr16(ip + 4, d->ip_id++);
  ip[8] = 64;
  ip[9] = 17;
  wr32n(ip + 12, d->server_ip);
  wr32n(ip + 16, dst);
  wr16(ip + 10, cksum_fold(cksum_add(0, ip, 20)));

  // UDP checksum with pseudo header
  uint8_t pseudo[12];
  memcpy(pseudo, ip + 12, 8);
  pseudo[8] = 0;
  pseudo[9] = 17;
  wr16(pseudo + 10, 8 + dhcp_len);
  uint32_t sum = cksum_add(0, pseudo, 12);
  sum = cksum_add(sum, udp, 8 + dhcp_len);
  uint16_t c = cksum_fold(sum);
  wr16(udp + 6, c ? c : 0xffff);

  switch(msgtype) {
  case DHCPOFFER: d->tx_offer++; break;
  case DHCPACK:   d->tx_ack++;   break;
  case DHCPNAK:   d->tx_nak++;   break;
  }
  sim_dhcpd_send(d, f, 14 + 20 + 8 + dhcp_len);
}


static void
sim_dhcpd_arp_reply(sim_dhcpd_t *d, const uint8_t *req)
{
  uint8_t f[42];
  memcpy(f, req + 6, 6);        // to requester
  memcpy(f + 6, d->mac, 6);
  wr16(f + 12, ETHERTYPE_ARP);
  uint8_t *a = f + 14;
  wr16(a + 0, 1);               // Ethernet
  wr16(a + 2, ETHERTYPE_IPV4);
  a[4] = 6;
  a[5] = 4;
  wr16(a + 6, 2);               // reply
  memcpy(a + 8, d->mac, 6);
  wr32n(a + 14, d->server_ip);
  memcpy(a + 18, req + 14 + 8, 10); // target hw + proto addr = requester
  vnet_peer_send(d->vnet, f, sizeof(f));
}


// ---- Receive ----

static void
sim_dhcpd_dhcp(sim_dhcpd_t *d, const uint8_t *frame, const uint8_t *dh,
               size_t len)
{
  if(len < 240 || dh[0] != 1)
    return;
  if(dh[236] != 0x63 || dh[237] != 0x82 || dh[238] != 0x53 || dh[239] != 0x63)
    return;

  const uint8_t *client_mac = dh + 28;
  const uint32_t xid = rd32n(dh + 4);
  int msgtype = 0;
  uint32_t requested_ip = 0, server_id = 0;

  // Options
  const uint8_t *o = dh + 240;
  const uint8_t *end = dh + len;
  while(o + 2 <= end && *o != 255) {
    if(*o == 0) {
      o++;
      continue;
    }
    const uint8_t type = o[0], olen = o[1];
    if(o + 2 + olen > end)
      break;
    switch(type) {
    case 53: if(olen == 1) msgtype = o[2]; break;
    case 50: if(olen == 4) requested_ip = rd32n(o + 2); break;
    case 54: if(olen == 4) server_id = rd32n(o + 2); break;
    }
    o += 2 + olen;
  }

  d->last_xid = xid;
  d->last_ciaddr = rd32n(dh + 12);
  d->last_requested_ip = requested_ip;
  d->last_server_id = server_id;

  switch(msgtype) {
  case DHCPDISCOVER:
    d->rx_discover++;
    if(d->silent)
      return;
    if(d->ignore_discovers > 0) {
      d->ignore_discovers--;
      return;
    }
    sim_dhcpd_reply(d, DHCPOFFER, client_mac, xid, d->pool_ip);
    break;

  case DHCPREQUEST:
    d->rx_request++;
    if(!memcmp(frame, d->mac, 6))
      d->rx_request_unicast++;
    if(d->silent)
      return;
    if(d->nak_requests > 0) {
      d->nak_requests--;
      sim_dhcpd_reply(d, DHCPNAK, client_mac, xid, 0);
      return;
    }
    // Renewal carries the address in ciaddr, initial request in option 50
    const uint32_t want = requested_ip ? requested_ip : d->last_ciaddr;
    if(want != d->pool_ip) {
      sim_dhcpd_reply(d, DHCPNAK, client_mac, xid, 0);
      return;
    }
    sim_dhcpd_reply(d, DHCPACK, client_mac, xid, d->pool_ip);
    break;
  }
}


static void
sim_dhcpd_rx(sim_dhcpd_t *d, const uint8_t *frame, size_t len)
{
  if(len < 14)
    return;
  const uint16_t etype = rd16(frame + 12);

  if(etype == ETHERTYPE_ARP && len >= 42) {
    const uint8_t *a = frame + 14;
    if(rd16(a + 6) == 1 && rd32n(a + 24) == d->server_ip) {
      d->rx_arp_request++;
      sim_dhcpd_arp_reply(d, frame);
    }
    return;
  }

  if(etype != ETHERTYPE_IPV4 || len < 14 + 20 + 8)
    return;
  const uint8_t *ip = frame + 14;
  if(ip[0] != 0x45 || ip[9] != 17)
    return;
  const size_t iplen = rd16(ip + 2);
  if(iplen < 28 || 14 + iplen > len)
    return;
  const uint8_t *udp = ip + 20;
  if(rd16(udp + 2) != 67)
    return;
  const size_t udplen = rd16(udp + 4);
  if(udplen < 8 || udplen > iplen - 20)
    return;
  sim_dhcpd_dhcp(d, frame, udp + 8, udplen - 8);
}


// ---- The server thread ----

static void
sim_dhcpd_thread(void *arg)
{
  sim_dhcpd_t *d = arg;
  uint8_t *frame = d->rxframe;

  while(1) {
    const uint64_t deadline = d->pending_len ? d->pending_at : SIM_NEVER;
    const ssize_t n = vnet_peer_recv(d->vnet, frame, sizeof(d->rxframe), deadline);
    if(n < 0) {
      // Deadline: send the delayed reply
      vnet_peer_send(d->vnet, d->pending, d->pending_len);
      d->pending_len = 0;
      continue;
    }
    sim_dhcpd_rx(d, frame, n);
  }
}


// ---- Setup ----

static uint32_t
ip4(int a, int b, int c, int dd)
{
  uint8_t v[4] = { a, b, c, dd };
  return rd32n(v);
}

void
sim_dhcpd_defaults(sim_dhcpd_t *d, int net)
{
  memset(d, 0, sizeof(*d));
  const uint8_t mac[6] = { 0x02, 0xd0, 0x00, 0x00, net, 0x01 };
  memcpy(d->mac, mac, 6);
  d->server_ip = ip4(10, net, 0, 1);
  d->gateway = ip4(10, net, 0, 1);
  d->netmask = ip4(255, 255, 255, 0);
  d->pool_ip = ip4(10, net, 0, 50);
  d->lease_time = 60;
}

vnet_t *
sim_dhcpd_attach(sim_dhcpd_t *d, const char *ifname, const uint8_t client_mac[6])
{
  d->vnet = vnet_create(ifname, client_mac);
  sim_thread_create("dhcpd", sim_dhcpd_thread, d, 0);
  return d->vnet;
}
