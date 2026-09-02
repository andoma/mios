/*
 * dhcp-client: drive src/net/ipv4/dhcpv4.c through its state machine
 * against a simulated server, in virtual time.
 *
 * Each scenario gets its own vnet on its own /24 so interfaces from
 * earlier scenarios (which keep renewing in the background) cannot
 * interfere with routing of unicast renewals. Scenario state is heap
 * allocated and never freed for the same reason: the netif and its
 * server model outlive the scenario function.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <net/ether.h>

#include "hosttest.h"
#include "sim_dhcpd.h"

typedef struct scenario {
  sim_dhcpd_t srv;
  vnet_t *vnet;
  ether_netif_t *eni;
  int net;
} scenario_t;

#define SEC 1000000ull


static scenario_t *
scenario_begin(int net, const char *title)
{
  hosttest_log("---- %s ----", title);
  scenario_t *sc = calloc(1, sizeof(scenario_t));
  sc->net = net;
  sim_dhcpd_defaults(&sc->srv, net);
  return sc;
}

// Link up starts the client
static void
scenario_start(scenario_t *sc)
{
  const uint8_t client_mac[6] = { 0x02, 0xc0, 0x00, 0x00, sc->net, 0x02 };
  char ifname[8];
  snprintf(ifname, sizeof(ifname), "veth%d", sc->net);
  sc->vnet = sim_dhcpd_attach(&sc->srv, ifname, client_mac);
  sc->eni = vnet_eni(sc->vnet);
  vnet_set_link(sc->vnet, 1);
}

static int
pred_bound(void *arg)
{
  scenario_t *sc = arg;
  return sc->eni->eni_ni.ni_ipv4_local_addr == sc->srv.pool_ip;
}

static int
pred_unbound(void *arg)
{
  scenario_t *sc = arg;
  return sc->eni->eni_ni.ni_ipv4_local_addr == 0;
}

static int
pred_acks_ge2(void *arg)
{
  scenario_t *sc = arg;
  return sc->srv.tx_ack >= 2;
}

static int
pred_discover_ge2(void *arg)
{
  scenario_t *sc = arg;
  return sc->srv.rx_discover >= 2;
}

static void
report(scenario_t *sc)
{
  const sim_dhcpd_t *s = &sc->srv;
  hosttest_log("  discover:%d request:%d (unicast:%d) offer:%d ack:%d nak:%d arp:%d",
               s->rx_discover, s->rx_request, s->rx_request_unicast,
               s->tx_offer, s->tx_ack, s->tx_nak, s->rx_arp_request);
}


// 1. Plain DISCOVER/OFFER/REQUEST/ACK
static void
test_basic(void)
{
  scenario_t *sc = scenario_begin(1, "basic");
  scenario_start(sc);

  const uint64_t t0 = clock_get();
  CHECK(hosttest_wait(pred_bound, sc, 2 * SEC), "client did not bind");
  const uint64_t took = clock_get() - t0;
  report(sc);

  CHECK(sc->srv.rx_discover == 1, "discovers=%d", sc->srv.rx_discover);
  CHECK(sc->srv.rx_request == 1, "requests=%d", sc->srv.rx_request);
  CHECK(sc->srv.tx_offer == 1 && sc->srv.tx_ack == 1, "offer=%d ack=%d",
        sc->srv.tx_offer, sc->srv.tx_ack);
  CHECK(sc->srv.last_requested_ip == sc->srv.pool_ip, "REQUEST option 50");
  CHECK(sc->srv.last_server_id == sc->srv.server_ip, "REQUEST option 54");
  CHECK(sc->eni->eni_ni.ni_ipv4_local_prefixlen == 24, "prefixlen=%d",
        sc->eni->eni_ni.ni_ipv4_local_prefixlen);
  CHECK(took < 100000, "binding took %d us", (int)took);

  // Renewal timer at half the lease
  const int64_t renew_in = sc->eni->eni_dhcp_timer.t_expire - clock_get();
  CHECK(renew_in > 29 * (int64_t)SEC && renew_in <= 30 * (int64_t)SEC,
        "renew timer in %d us, expected ~30 s", (int)renew_in);
}


// 2. Server ignores the first DISCOVERs: client retries every 500 ms
static void
test_slow_server(void)
{
  scenario_t *sc = scenario_begin(2, "slow-server");
  sc->srv.ignore_discovers = 3;
  scenario_start(sc);

  const uint64_t t0 = clock_get();
  CHECK(hosttest_wait(pred_bound, sc, 5 * SEC), "client did not bind");
  const uint64_t took = clock_get() - t0;
  report(sc);

  CHECK(sc->srv.rx_discover == 4, "discovers=%d", sc->srv.rx_discover);
  CHECK(took >= 1500000 && took < 1700000,
        "bound after %d us, expected ~1.5 s", (int)took);
}


// 3. NAK on the first REQUEST sends the client back to DISCOVER
static void
test_nak(void)
{
  scenario_t *sc = scenario_begin(3, "nak");
  sc->srv.nak_requests = 1;
  scenario_start(sc);

  CHECK(hosttest_wait(pred_bound, sc, 5 * SEC), "client did not bind");
  report(sc);

  CHECK(sc->srv.tx_nak == 1, "naks=%d", sc->srv.tx_nak);
  CHECK(sc->srv.rx_discover == 2, "discovers=%d (NAK must restart discovery)",
        sc->srv.rx_discover);
  CHECK(sc->srv.rx_request == 2, "requests=%d", sc->srv.rx_request);
  CHECK(sc->srv.tx_ack == 1, "acks=%d", sc->srv.tx_ack);
}


// 4. Renewal at T1: unicast REQUEST to the server, needs ARP first
static void
test_renew(void)
{
  scenario_t *sc = scenario_begin(4, "renew");
  sc->srv.lease_time = 20;
  scenario_start(sc);

  CHECK(hosttest_wait(pred_bound, sc, 2 * SEC), "client did not bind");
  const uint64_t t_bound = clock_get();

  CHECK(hosttest_wait(pred_acks_ge2, sc, 15 * SEC), "no renewal ACK");
  const uint64_t renew_after = clock_get() - t_bound;
  report(sc);

  CHECK(renew_after >= 10 * SEC && renew_after < 11 * SEC,
        "renewed after %d us, expected ~10 s", (int)renew_after);
  CHECK(sc->srv.rx_request_unicast >= 1, "renewal was not unicast");
  CHECK(sc->srv.rx_arp_request >= 1, "client did not ARP for the server");
  CHECK(sc->srv.last_ciaddr == sc->srv.pool_ip, "renewal ciaddr");
  CHECK(sc->srv.rx_discover == 1, "discovers=%d, renewal must not rediscover",
        sc->srv.rx_discover);
  CHECK(pred_bound(sc), "address lost across renewal");
}


// 5. Server vanishes at renewal: 5 REQUEST retries with backoff, then
//    back to DISCOVER with the address dropped, rebinding when it returns
static void
test_renew_silent(void)
{
  scenario_t *sc = scenario_begin(5, "renew-silent");
  sc->srv.lease_time = 20;
  scenario_start(sc);

  CHECK(hosttest_wait(pred_bound, sc, 2 * SEC), "client did not bind");
  sc->srv.silent = 1;

  CHECK(hosttest_wait(pred_discover_ge2, sc, 30 * SEC),
        "client never fell back to DISCOVER");
  const uint64_t t_rediscover = clock_get();
  report(sc);

  // 1 initial REQUEST + 5 retries (0.5+1+1.5+2+2.5 s = 7.5 s) after T1=10 s
  CHECK(sc->srv.rx_request == 6, "requests=%d, expected 1 + 5 retries",
        sc->srv.rx_request);
  CHECK(pred_unbound(sc), "address kept after giving up on the server");

  sc->srv.silent = 0;
  CHECK(hosttest_wait(pred_bound, sc, 2 * SEC), "client did not rebind");
  hosttest_log("  rebound %d us after rediscovery", (int)(clock_get() - t_rediscover));
  report(sc);
  CHECK(sc->srv.tx_ack == 2, "acks=%d", sc->srv.tx_ack);
}


// 6. Link flap while bound: client re-REQUESTs, keeps its address
static void
test_link_flap(void)
{
  scenario_t *sc = scenario_begin(6, "link-flap");
  scenario_start(sc);

  CHECK(hosttest_wait(pred_bound, sc, 2 * SEC), "client did not bind");

  vnet_set_link(sc->vnet, 0);
  usleep(100000);
  vnet_set_link(sc->vnet, 1);

  CHECK(hosttest_wait(pred_acks_ge2, sc, 3 * SEC), "no ACK after link up");
  report(sc);
  CHECK(sc->srv.rx_discover == 1, "discovers=%d", sc->srv.rx_discover);
  CHECK(pred_bound(sc), "address lost across link flap");
}


// 7. Slow server, replying just inside the 500 ms retransmit interval
static void
test_delayed_server(void)
{
  scenario_t *sc = scenario_begin(7, "delayed-server");
  sc->srv.reply_delay = 400000;
  scenario_start(sc);

  CHECK(hosttest_wait(pred_bound, sc, 5 * SEC), "client did not bind");
  report(sc);
  CHECK(sc->srv.rx_discover == 1, "discovers=%d", sc->srv.rx_discover);
  CHECK(sc->srv.tx_offer == 1, "offers=%d", sc->srv.tx_offer);
}


// 8. Informational: a server slower than the retransmit interval. Each
//    retransmitted DISCOVER carries a fresh xid, so every OFFER arrives
//    stale and the client never binds. RFC 2131 permits either keeping
//    or changing xid on retransmit; this documents what Mios does.
static void
test_slower_than_retry(void)
{
  scenario_t *sc = scenario_begin(8, "slower-than-retry (informational)");
  sc->srv.reply_delay = 600000;
  scenario_start(sc);

  const int bound = hosttest_wait(pred_bound, sc, 3 * SEC);
  report(sc);
  hosttest_log("  600 ms server: %s after 3 s (%d OFFERs discarded as stale)",
               bound ? "bound" : "NOT bound", sc->srv.tx_offer - bound);
}


static int
run_dhcp_client(void)
{
  test_basic();
  test_slow_server();
  test_nak();
  test_renew();
  test_renew_silent();
  test_link_flap();
  test_delayed_server();
  test_slower_than_retry();
  return 0; // failures are counted by CHECK()
}

HOSTTEST_SUITE("dhcp-client", run_dhcp_client, 0);
