/*
 * dns-client: src/net/ipv4/dns.c against the DNS server in sim_dhcpd,
 * in virtual time. The client learns the server over DHCP like it
 * would on a real network.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <net/ether.h>
#include <net/ipv4/dns.h>

#include "hosttest.h"
#include "sim_dhcpd.h"

#define SEC 1000000ull

static uint32_t
ip4(int a, int b, int c, int d)
{
  const uint8_t v[4] = { a, b, c, d };
  uint32_t r;
  memcpy(&r, v, 4);
  return r;
}

static sim_dns_record_t records[] = {
  { "a.test" },
  { "www.test", 0, 60, "a.test" },
  { "short.test", 0, 5 },
  { "lossy.test" },
  // Ten 45 byte records before the A make a 492 byte answer, which
  // doesn't fit in the first 512 byte pbuf after the headers
  { "big.test", 0, 60, NULL, 10 },
};

static int
pred_bound(void *arg)
{
  const ether_netif_t *eni = arg;
  return eni->eni_ni.ni_ipv4_local_addr != 0;
}


static int
run_dns_client(void)
{
  for(int i = 0; i < 5; i++) {
    records[i].addr = ip4(192, 0, 2, 1 + i);
    if(!records[i].ttl)
      records[i].ttl = 60;
  }

  sim_dhcpd_t *srv = calloc(1, sizeof(sim_dhcpd_t));
  sim_dhcpd_defaults(srv, 1);
  srv->dns_server = srv->server_ip;
  srv->dns_records = records;
  srv->dns_num_records = sizeof(records) / sizeof(records[0]);

  const uint8_t client_mac[6] = { 0x02, 0xc0, 0x00, 0x00, 0x01, 0x02 };
  vnet_t *vn = sim_dhcpd_attach(srv, "veth1", client_mac);
  ether_netif_t *eni = vnet_eni(vn);
  vnet_set_link(vn, 1);

  CHECK(hosttest_wait(pred_bound, eni, 2 * SEC), "client did not bind");
  CHECK(eni->eni_ni.ni_ipv4_dns[0] == srv->server_ip,
        "DNS server not learned from DHCP");

  uint32_t addr = 0;
  error_t err;
  uint64_t t0;

  hosttest_log("---- literal ----");
  err = dns_resolve("10.9.8.7", &addr);
  CHECK(!err && addr == ip4(10, 9, 8, 7), "literal: err=%d", err);
  CHECK(srv->rx_dns_query == 0, "literal must not query");

  hosttest_log("---- basic ----");
  err = dns_resolve("a.test", &addr);
  CHECK(!err && addr == records[0].addr, "a.test: err=%d", err);
  CHECK(srv->rx_dns_query == 1, "queries=%d", srv->rx_dns_query);

  hosttest_log("---- cached ----");
  err = dns_resolve("A.Test", &addr);
  CHECK(!err && addr == records[0].addr, "A.Test: err=%d", err);
  CHECK(srv->rx_dns_query == 1, "cache miss, queries=%d", srv->rx_dns_query);

  hosttest_log("---- cname ----");
  err = dns_resolve("www.test", &addr);
  CHECK(!err && addr == records[1].addr, "www.test: err=%d", err);

  hosttest_log("---- nxdomain ----");
  err = dns_resolve("nothere.test", &addr);
  CHECK(err == ERR_NOT_FOUND, "nothere.test: err=%d", err);

  hosttest_log("---- invalid names ----");
  CHECK(dns_resolve("", &addr) == ERR_INVALID_ARGS, "empty name");
  CHECK(dns_resolve("a..test", &addr) == ERR_INVALID_ARGS, "empty label");

  hosttest_log("---- lost query ----");
  int q0 = srv->rx_dns_query;
  srv->dns_drop = 1;
  t0 = clock_get();
  err = dns_resolve("lossy.test", &addr);
  uint64_t took = clock_get() - t0;
  CHECK(!err && addr == records[3].addr, "lossy.test: err=%d", err);
  CHECK(srv->rx_dns_query - q0 == 2, "queries=%d", srv->rx_dns_query - q0);
  CHECK(took >= SEC && took < SEC + 100000, "took %d us", (int)took);

  hosttest_log("---- ttl ----");
  err = dns_resolve("short.test", &addr);
  CHECK(!err && addr == records[2].addr, "short.test: err=%d", err);
  q0 = srv->rx_dns_query;
  usleep(4 * SEC);
  err = dns_resolve("short.test", &addr);
  CHECK(!err && srv->rx_dns_query == q0, "expired before its TTL");
  usleep(2 * SEC);
  err = dns_resolve("short.test", &addr);
  CHECK(!err && srv->rx_dns_query == q0 + 1, "not expired after its TTL");

  hosttest_log("---- answer spanning pbufs ----");
  err = dns_resolve("big.test", &addr);
  CHECK(!err && addr == records[4].addr, "big.test: err=%d addr=%08x",
        err, (unsigned)addr);

  hosttest_log("---- timeout ----");
  q0 = srv->rx_dns_query;
  srv->dns_drop = 100;
  t0 = clock_get();
  err = dns_resolve("lost.test", &addr);
  took = clock_get() - t0;
  srv->dns_drop = 0;
  CHECK(err == ERR_TIMEOUT, "lost.test: err=%d", err);
  CHECK(srv->rx_dns_query - q0 == 3, "queries=%d", srv->rx_dns_query - q0);
  CHECK(took >= 5 * SEC && took < 5 * SEC + 100000, "took %d us", (int)took);

  return 0; // failures are counted by CHECK()
}

HOSTTEST_SUITE_EX("dns-client", run_dns_client, 0, 512);
