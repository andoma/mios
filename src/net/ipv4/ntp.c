#include "udp.h"

#include "net/pbuf.h"
#include "net/ether.h"
#include "net/ipv4/ipv4.h"
#include "net/ipv4/udp.h"
#include "net/net.h"
#include "net/net_task.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <mios/cli.h>
#include <mios/datetime.h>

typedef struct {
  uint32_t seconds;
  uint32_t fractions;
} ntp_ts_t;

typedef struct {
  uint8_t flags;
  uint8_t stratum;
  uint8_t poll;
  int8_t precision;
  uint32_t root_delay;
  uint32_t root_dispersion;
  uint32_t reference_id;

  ntp_ts_t ref_ts;
  ntp_ts_t origin_ts;
  ntp_ts_t rx_ts;
  ntp_ts_t tx_ts;

} ntp_pkt_t;


static uint32_t ntp_server;
static int64_t ntp_xmit_time;
static void ntp_timer_cb(void *opaque, uint64_t expire);

static timer_t ntp_timer = {
  .t_cb = ntp_timer_cb,
  .t_name = "NTP"
};

static uint64_t
usec_from_ntp_ts(const ntp_ts_t *ts)
{
  uint32_t seconds = ntohl(ts->seconds) - 2208988800;
  uint32_t us = ((uint64_t)ntohl(ts->fractions) * 1000000) >> 32;
  return (uint64_t)seconds * 1000000 + us;
}


static void
ntp_ts_from_usec(ntp_ts_t *nt, uint64_t usec)
{
  uint32_t seconds = usec / 1000000;
  uint32_t useconds = usec % 1000000;
  uint32_t fractions = ((uint64_t)useconds * 281475258) >> 16;

  nt->seconds = htonl(seconds + 2208988800);
  nt->fractions = htonl(fractions);
}


static pbuf_t *
ntp_input(struct netif *ni, pbuf_t *pb, size_t udp_offset)
{
  int64_t now = clock_get();

  const ipv4_header_t *ip = pbuf_data(pb, 0);
  const uint32_t from = ip->src_addr;

  if(from != ntp_server)
    return pb;

  pb = pbuf_drop(pb, udp_offset + 8, 0);

  const ntp_pkt_t *np = pbuf_cdata(pb, 0);
  int64_t server_rx = usec_from_ntp_ts(&np->rx_ts);
  int64_t server_tx = usec_from_ntp_ts(&np->tx_ts);
  int64_t theta = ((server_rx - ntp_xmit_time) + (server_tx - now)) / 2;
  datetime_set_utc_offset(theta, "NTP");
  return pb;
}


static void
ntp_send(void)
{
  pbuf_t *pb = pbuf_make(16 + 20 + 8, 0); // Make space for ether + ip + udp
  if(pb == NULL)
    return;
  ntp_pkt_t *np = pbuf_append(pb, sizeof(ntp_pkt_t));
  memset(np, 0, sizeof(ntp_pkt_t));
  np->flags = 0x23;
  np->precision = -18; // 1µS

  uint64_t now = clock_get();

  if(wallclock.utc_offset) {
    ntp_ts_from_usec(&np->origin_ts, wallclock.utc_offset + now);
  }

  ntp_xmit_time = now;
  net_timer_arm(&ntp_timer, now + 15000000);
  udp_send(NULL, pb, ntp_server, NULL, 123, 123);
}


static void
ntp_timer_cb(void *opaque, uint64_t expire)
{
  ntp_send();
}

UDP_INPUT(ntp_input, 123);

static void
ntp_cb(struct net_task *nt, uint32_t signals)
{
  ntp_send();
}

static net_task_t ntp_task = { ntp_cb };

void
ntp_set_server(uint32_t server_addr)
{
  if(ntp_server == server_addr)
    return;

  ntp_server = server_addr;
  net_task_raise(&ntp_task, 1);
}


static error_t
cmd_set_ntp_server(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  ntp_set_server(inet_addr(argv[1]));
  return 0;
}

CLI_CMD_DEF("ntp", cmd_set_ntp_server);
