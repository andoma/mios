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
  // Queue it; the receive loop sends it when its time comes. Constant
  // delay means arrival order is send order, so a ring is enough.
  const int n = sizeof(d->pending) / sizeof(d->pending[0]);
  if(d->pending_wr - d->pending_rd == n)
    return; // Full, drop
  __typeof__(d->pending[0]) *p = &d->pending[d->pending_wr % n];
  memcpy(p->frame, frame, len);
  p->len = len;
  p->at = clock_get() + d->reply_delay;
  d->pending_wr++;
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
    if(d->dns_server) {
      *o++ = 6; *o++ = 4; wr32n(o, d->dns_server); o += 4;
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


// Fill in IPv4 and UDP headers (and checksums) of a reply to 'req'
static void
sim_dhcpd_udp_reply_headers(sim_dhcpd_t *d, uint8_t *f, const uint8_t *req,
                            size_t payload_len)
{
  const uint8_t *rip = req + 14;
  const uint8_t *rudp = rip + 20;

  memcpy(f, req + 6, 6);
  memcpy(f + 6, d->mac, 6);
  wr16(f + 12, ETHERTYPE_IPV4);

  uint8_t *ip = f + 14;
  memset(ip, 0, 20);
  ip[0] = 0x45;
  wr16(ip + 2, 20 + 8 + payload_len);
  wr16(ip + 4, d->ip_id++);
  ip[8] = 64;
  ip[9] = 17;
  wr32n(ip + 12, d->server_ip);
  memcpy(ip + 16, rip + 12, 4);
  wr16(ip + 10, cksum_fold(cksum_add(0, ip, 20)));

  uint8_t *udp = ip + 20;
  wr16(udp + 0, rd16(rudp + 2));
  wr16(udp + 2, rd16(rudp + 0));
  wr16(udp + 4, 8 + payload_len);
  wr16(udp + 6, 0);

  uint8_t pseudo[12];
  memcpy(pseudo, ip + 12, 8);
  pseudo[8] = 0;
  pseudo[9] = 17;
  wr16(pseudo + 10, 8 + payload_len);
  uint32_t sum = cksum_add(0, pseudo, 12);
  sum = cksum_add(sum, udp, 8 + payload_len);
  const uint16_t c = cksum_fold(sum);
  wr16(udp + 6, c ? c : 0xffff);
}


// Decode the (uncompressed) question name into dotted form
static int
sim_dns_qname(const uint8_t *q, const uint8_t *end, char *out, size_t outlen)
{
  size_t n = 0;
  while(q < end && *q) {
    const int len = *q++;
    if(q + len > end || n + len + 2 > outlen)
      return -1;
    if(n)
      out[n++] = '.';
    memcpy(out + n, q, len);
    n += len;
    q += len;
  }
  if(q >= end)
    return -1;
  out[n] = 0;
  return 0;
}


static uint8_t *
sim_dns_name(uint8_t *o, const char *name)
{
  while(*name) {
    const char *dot = strchr(name, '.');
    const size_t len = dot ? (size_t)(dot - name) : strlen(name);
    *o++ = len;
    memcpy(o, name, len);
    o += len;
    name += len;
    if(*name == '.')
      name++;
  }
  *o++ = 0;
  return o;
}


static uint8_t *
sim_dns_rr(uint8_t *o, int type, uint32_t ttl, const void *rdata, int rdlen)
{
  wr16(o, 0xc00c); // Name: pointer to the question
  wr16(o + 2, type);
  wr16(o + 4, 1);
  wr16(o + 6, ttl >> 16);
  wr16(o + 8, ttl);
  wr16(o + 10, rdlen);
  memcpy(o + 12, rdata, rdlen);
  return o + 12 + rdlen;
}


static void
sim_dhcpd_dns(sim_dhcpd_t *d, const uint8_t *frame, const uint8_t *q,
              size_t len)
{
  if(len < 12 + 5 || (q[2] & 0x80))
    return;
  d->rx_dns_query++;
  if(d->dns_drop > 0) {
    d->dns_drop--;
    return;
  }

  char name[256];
  const uint8_t *qend = q + len;
  if(sim_dns_qname(q + 12, qend - 4, name, sizeof(name)))
    return;
  const size_t qlen = len; // Header and the question, no other sections

  const sim_dns_record_t *r = NULL;
  for(int i = 0; i < d->dns_num_records; i++) {
    if(!strcmp(d->dns_records[i].name, name))
      r = &d->dns_records[i];
  }

  uint8_t *f = d->dnsframe;
  uint8_t *m = f + 42;
  memcpy(m, q, qlen);
  wr16(m + 2, 0x8180 | (r ? 0 : 3)); // Response, RD, RA, NXDOMAIN
  wr16(m + 6, 0);
  uint8_t *o = m + qlen;
  int answers = 0;

  if(r != NULL) {
    for(int i = 0; i < r->padding; i++) {
      static const uint8_t txt[33] = "\x20padding padding padding paddin";
      o = sim_dns_rr(o, 16, r->ttl, txt, sizeof(txt));
      answers++;
    }
    if(r->cname) {
      uint8_t target[256];
      const int tlen = sim_dns_name(target, r->cname) - target;
      o = sim_dns_rr(o, 5, r->ttl, target, tlen);
      answers++;
    }
    o = sim_dns_rr(o, 1, r->ttl, &r->addr, 4);
    answers++;
  }
  wr16(m + 6, answers);

  const size_t mlen = o - m;
  sim_dhcpd_udp_reply_headers(d, f, frame, mlen);
  sim_dhcpd_send(d, f, 42 + mlen);
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
    if(d->rx_discover && xid != d->last_discover_xid)
      d->discover_xid_changes++;
    d->last_discover_xid = xid;
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
    if(d->rx_request && xid != d->last_request_xid)
      d->request_xid_changes++;
    d->last_request_xid = xid;
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
  const size_t udplen = rd16(udp + 4);
  if(udplen < 8 || udplen > iplen - 20)
    return;
  if(rd16(udp + 2) == 67)
    sim_dhcpd_dhcp(d, frame, udp + 8, udplen - 8);
  else if(rd16(udp + 2) == 53 && d->dns_num_records &&
          rd32n(ip + 16) == d->server_ip)
    sim_dhcpd_dns(d, frame, udp + 8, udplen - 8);
}


// ---- The server thread ----

static void
sim_dhcpd_thread(void *arg)
{
  sim_dhcpd_t *d = arg;
  uint8_t *frame = d->rxframe;

  const int nslots = sizeof(d->pending) / sizeof(d->pending[0]);

  while(1) {
    // Send every delayed reply that is due, then wait for the next
    // frame or the next due time, whichever comes first.
    uint64_t deadline = SIM_NEVER;
    while(d->pending_rd != d->pending_wr) {
      __typeof__(d->pending[0]) *p = &d->pending[d->pending_rd % nslots];
      if(p->at > clock_get()) {
        deadline = p->at;
        break;
      }
      vnet_peer_send(d->vnet, p->frame, p->len);
      d->pending_rd++;
    }
    const ssize_t n = vnet_peer_recv(d->vnet, frame, sizeof(d->rxframe), deadline);
    if(n >= 0)
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
