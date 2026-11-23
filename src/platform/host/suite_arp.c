/*
 * arp: drive the nexthop and ARP layer (src/net/ether.c, the nexthops of
 * src/net/ipv4/ipv4.c) against a simulated server that answers ARP for
 * its address, or stays silent, and counts what is sent to it by the
 * MAC it went to. A frame for the server at any MAC but the server's is
 * one Mios sent to an address it never resolved: what these scenarios
 * must never see.
 *
 * The server is the DHCP server of sim_dhcpd.c, which is also how the
 * field met this: a board's leader is its DHCP server, broker and
 * governor at one address.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <net/ether.h>
#include <net/pbuf.h>
#include <net/net_task.h>
#include <net/ipv4/ipv4.h>
#include <net/ipv4/udp.h>

#include "hosttest.h"
#include "sim_dhcpd.h"

#define SEC 1000000ull

typedef struct scenario {
  sim_dhcpd_t srv;
  vnet_t *vnet;
  ether_netif_t *eni;
} scenario_t;


static int
pred_bound(void *arg)
{
  scenario_t *sc = arg;
  return sc->eni->eni_ni.ni_ipv4_local_addr == sc->srv.pool_ip;
}

// A bound interface on its own /24, its server answering ARP. Heap
// allocated and never freed: the netif and the server outlive the
// scenario (see suite_dhcp.c).
static scenario_t *
scenario_begin(int net, const char *title)
{
  hosttest_log("---- %s ----", title);
  scenario_t *sc = calloc(1, sizeof(scenario_t));
  sim_dhcpd_defaults(&sc->srv, net);
  const uint8_t mac[6] = { 0x02, 0xa0, 0x00, 0x00, net, 0x02 };
  char ifname[8];
  snprintf(ifname, sizeof(ifname), "arp%d", net);
  sc->vnet = sim_dhcpd_attach(&sc->srv, ifname, mac);
  sc->eni = vnet_eni(sc->vnet);
  vnet_set_link(sc->vnet, 1);
  hosttest_wait(pred_bound, sc, 10 * SEC);
  return sc;
}


// ---- Running code on the net thread, where the nexthops live ----

typedef struct net_call {
  net_task_t task;
  void (*fn)(scenario_t *sc, int arg);
  scenario_t *sc;
  int arg;
  int done;
} net_call_t;

static void
net_call_cb(net_task_t *nt, uint32_t signals)
{
  net_call_t *nc = (net_call_t *)nt;
  nc->fn(nc->sc, nc->arg);
  nc->done = 1;
}

static int
pred_call_done(void *arg)
{
  return ((net_call_t *)arg)->done;
}

static void
on_net(void (*fn)(scenario_t *sc, int arg), scenario_t *sc, int arg)
{
  net_call_t nc = { .task.nt_cb = net_call_cb, .fn = fn, .sc = sc,
                    .arg = arg };
  net_task_raise(&nc.task, 1);
  hosttest_wait(pred_call_done, &nc, SEC);
}

// `count` UDP datagrams to the server, back to back
static void
send_udp(scenario_t *sc, int count)
{
  for(int i = 0; i < count; i++) {
    pbuf_t *pb = pbuf_make(16 + 20 + 8, 0);
    if(pb == NULL)
      return;
    uint8_t *p = pbuf_append(pb, 4);
    memcpy(p, "arp!", 4);
    udp_send(NULL, pb, sc->srv.server_ip, NULL, 4000, 9);
  }
}

// Look the server up, as a sender does before its first packet, and
// send nothing: tcp_do_connect() does this before a SYN that
// tcp_emit() may not get a buffer for yet
static void
resolve_only(scenario_t *sc, int unused)
{
  ipv4_nexthop_resolve(sc->srv.server_ip);
}

static void
net_send(scenario_t *sc, int count)
{
  on_net(send_udp, sc, count);
}

static int
check_none_misaddressed(scenario_t *sc)
{
  const uint8_t *m = sc->srv.last_misaddressed_mac;
  return CHECK(sc->srv.rx_other_misaddressed == 0,
               "%d frame(s) for the server went to %02x:%02x:%02x:%02x:%02x:%02x,"
               " not the server's MAC",
               sc->srv.rx_other_misaddressed, m[0], m[1], m[2], m[3], m[4],
               m[5]);
}

static void
report(scenario_t *sc)
{
  const sim_dhcpd_t *s = &sc->srv;
  hosttest_log("  arp requests:%d  frames for the server:%d (to its MAC:%d,"
               " elsewhere:%d)",
               s->rx_arp_request, s->rx_other, s->rx_other_to_us,
               s->rx_other_misaddressed);
}


// ---- Scenarios ----

// The ordinary case: the first frame waits for ARP, later ones go
// straight to the learnt MAC
static int
scenario_resolves(void)
{
  int fails = 0;
  scenario_t *sc = scenario_begin(1, "first send resolves, then reuses");
  fails += !CHECK(pred_bound(sc), "never bound");

  net_send(sc, 1);
  usleep(SEC / 10);
  fails += !CHECK(sc->srv.rx_arp_request == 1, "%d ARP requests, want 1",
                  sc->srv.rx_arp_request);
  fails += !CHECK(sc->srv.rx_other_to_us == 1,
                  "the frame waiting for ARP was not delivered");

  usleep(2 * SEC);
  net_send(sc, 1);
  usleep(SEC / 10);
  fails += !CHECK(sc->srv.rx_arp_request == 1,
                  "resolved again although the entry was fresh (%d requests)",
                  sc->srv.rx_arp_request);
  fails += !CHECK(sc->srv.rx_other_to_us == 2, "second frame not delivered");
  fails += !check_none_misaddressed(sc);
  report(sc);
  return fails;
}

// An entry looked up and left unused while the once-a-second ageing
// runs: it must still be resolved before it is used. It used to count
// down from NEXTHOP_IDLE (0) to 255, which is what an ARP reply sets,
// and send to a MAC nobody had written (ethamp2, after a DFU deploy)
static int
scenario_unused_entry(void)
{
  int fails = 0;
  scenario_t *sc = scenario_begin(2, "an entry left unused across ageing");
  fails += !CHECK(pred_bound(sc), "never bound");

  on_net(resolve_only, sc, 0);
  usleep(3 * SEC + SEC / 2);
  net_send(sc, 1);
  usleep(SEC / 10);
  fails += !CHECK(sc->srv.rx_arp_request >= 1,
                  "sent without resolving the server first");
  fails += !CHECK(sc->srv.rx_other_to_us == 1, "frame not delivered");
  fails += !check_none_misaddressed(sc);
  report(sc);
  return fails;
}

// A server that does not answer: Mios keeps asking and never sends a
// frame for it to some other MAC. Sends while unresolved count the entry
// down from NEXTHOP_RESOLVE; reaching 0 between two ageing passes used
// to wrap it to "resolved" too. Once the server answers, frames flow
static int
scenario_silent_server(void)
{
  int fails = 0;
  scenario_t *sc = scenario_begin(3, "a server that does not answer ARP");
  fails += !CHECK(pred_bound(sc), "never bound");

  sc->srv.arp_silent = 1;
  net_send(sc, 8);
  for(int i = 0; i < 5; i++) {
    usleep(SEC);
    net_send(sc, 2);
  }
  usleep(SEC / 10);
  fails += !CHECK(sc->srv.rx_arp_request >= 3,
                  "stopped asking (%d ARP requests in 5 s)",
                  sc->srv.rx_arp_request);
  fails += !CHECK(sc->srv.rx_other_to_us == 0,
                  "delivered to a server that never answered");
  fails += !check_none_misaddressed(sc);

  sc->srv.arp_silent = 0;
  for(int i = 0; i < 3 && sc->srv.rx_other_to_us == 0; i++) {
    net_send(sc, 1);
    usleep(SEC + SEC / 10);
  }
  fails += !CHECK(sc->srv.rx_other_to_us >= 1,
                  "nothing delivered once the server answered");
  fails += !check_none_misaddressed(sc);
  report(sc);
  return fails;
}


static int
suite_arp(void)
{
  int fails = 0;
  fails += scenario_resolves();
  fails += scenario_unused_entry();
  fails += scenario_silent_server();
  return fails;
}

HOSTTEST_SUITE("arp", suite_arp, 0);
