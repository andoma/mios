// ICMP echo, the ping command

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mios/cli.h>
#include <mios/task.h>
#include <mios/bytestream.h>

#include "net/netif.h"
#include "net/net.h"
#include "net/pbuf.h"

#include "ipv4.h"

#define PING_PAYLOAD 56

// One ping command at a time
static mutex_t ping_cmd_mutex = MUTEX_INITIALIZER("pingcmd");

// Protected by ping_mutex
static mutex_t ping_mutex = MUTEX_INITIALIZER("ping");
static cond_t ping_cond = COND_INITIALIZER("ping");
static pbuf_t *ping_pb;     // Echo request handed to the net thread
static uint32_t ping_dst;
static uint16_t ping_id;
static uint16_t ping_seq;
static uint8_t ping_active; // Accept replies to ping_id/ping_seq
static uint8_t ping_ttl;
static int ping_result;     // 0 while waiting, reply length or an error
static int64_t ping_reply_time;


static void
ping_send_cb(net_task_t *nt, uint32_t signals)
{
  mutex_lock(&ping_mutex);
  pbuf_t *pb = ping_pb;
  ping_pb = NULL;
  const uint32_t dst = ping_dst;
  mutex_unlock(&ping_mutex);

  if(pb == NULL)
    return;

  nexthop_t *nh = ipv4_nexthop_resolve(dst);
  if(nh == NULL) {
    pbuf_free(pb);
    mutex_lock(&ping_mutex);
    ping_result = ERR_NO_ROUTE;
    cond_signal(&ping_cond);
    mutex_unlock(&ping_mutex);
    return;
  }
  netif_t *ni = nh->nh_netif;

  pb = pbuf_prepend(pb, sizeof(ipv4_header_t), 0, 0);
  if(pb == NULL)
    return;
  ipv4_header_t *ip = pbuf_data(pb, 0);

  ip->ver_ihl = 0x45;
  ip->tos = 0;
  ip->total_length = htons(pb->pb_pktlen);
  ip->id = rand();
  ip->fragment_info = 0;
  ip->ttl = 64;
  ip->proto = IPPROTO_ICMP;
  ip->src_addr = ni->ni_ipv4_local_addr;
  ip->dst_addr = dst;

  ip->cksum = 0;
  if(!(ni->ni_flags & NETIF_F_TX_IPV4_CKSUM_OFFLOAD))
    ip->cksum = ipv4_cksum_pbuf(0, pb, 0, sizeof(ipv4_header_t));

  ni->ni_output_ipv4(ni, nh, pb);
}

static net_task_t ping_task = { ping_send_cb };


// Called on the net thread for ICMP echo replies (checksum verified)
pbuf_t *
icmp_input_echo_reply(netif_t *ni, pbuf_t *pb, int icmp_offset)
{
  if(pbuf_pullup(pb, icmp_offset + 8))
    return pb;

  const ipv4_header_t *ip = pbuf_data(pb, 0);
  const uint8_t *icmp = pbuf_data(pb, icmp_offset);

  mutex_lock(&ping_mutex);
  if(ping_active && ping_result == 0 && ip->src_addr == ping_dst &&
     rd16_be(icmp + 4) == ping_id && rd16_be(icmp + 6) == ping_seq) {
    ping_reply_time = clock_get();
    ping_ttl = ip->ttl;
    ping_result = pb->pb_pktlen - icmp_offset;
    cond_signal(&ping_cond);
  }
  mutex_unlock(&ping_mutex);
  return pb;
}


static pbuf_t *
ping_make(uint16_t id, uint16_t seq)
{
  // Room for the Ethernet (and alignment) and IPv4 headers
  pbuf_t *pb = pbuf_make(16 + sizeof(ipv4_header_t), 1);
  uint8_t *icmp = pbuf_append(pb, 8 + PING_PAYLOAD);
  icmp[0] = 8; // Echo request
  icmp[1] = 0;
  wr16_be(icmp + 2, 0);
  wr16_be(icmp + 4, id);
  wr16_be(icmp + 6, seq);
  for(int i = 0; i < PING_PAYLOAD; i++)
    icmp[8 + i] = i;

  const uint16_t cksum = ipv4_cksum_pbuf(0, pb, 0, INT32_MAX);
  memcpy(icmp + 2, &cksum, 2);
  return pb;
}


static error_t
cmd_ping(cli_t *cli, int argc, char **argv)
{
  if(argc < 2 || argc > 3)
    return ERR_INVALID_ARGS;

  const uint32_t dst = inet_addr(argv[1]);
  const int count = argc > 2 ? atoi(argv[2]) : 4;
  if(dst == 0 || count < 1)
    return ERR_INVALID_ARGS;

  int received = 0;
  int64_t rtt_min = INT64_MAX, rtt_max = 0, rtt_sum = 0;
  error_t err = 0;

  mutex_lock(&ping_cmd_mutex);
  mutex_lock(&ping_mutex);
  ping_id = rand();
  ping_dst = dst;

  for(int seq = 1; seq <= count; seq++) {
    pbuf_t *pb = ping_make(ping_id, seq);

    pbuf_free(ping_pb); // Previous request, if the net thread never got to it
    ping_seq = seq;
    ping_result = 0;
    ping_active = 1;
    ping_pb = pb;
    const int64_t sent = clock_get();
    const int64_t deadline = sent + 1000000;
    net_task_raise(&ping_task, 1);

    while(ping_result == 0) {
      if(cond_wait_timeout(&ping_cond, &ping_mutex, deadline))
        break;
    }
    ping_active = 0;

    if(ping_result == ERR_NO_ROUTE) {
      cli_printf(cli, "No route to %Id\n", dst);
      err = ERR_NO_ROUTE;
      break;
    }

    if(ping_result > 0) {
      const int64_t rtt = ping_reply_time - sent;
      cli_printf(cli, "%d bytes from %Id: seq=%d ttl=%d time=%d.%03d ms\n",
                 ping_result, dst, seq, ping_ttl,
                 (int)(rtt / 1000), (int)(rtt % 1000));
      received++;
      rtt_sum += rtt;
      if(rtt < rtt_min)
        rtt_min = rtt;
      if(rtt > rtt_max)
        rtt_max = rtt;
    } else {
      cli_printf(cli, "Timeout seq=%d\n", seq);
    }

    if(seq < count) {
      // One request per second
      mutex_unlock(&ping_mutex);
      const int64_t now = clock_get();
      if(now < deadline)
        usleep(deadline - now);
      mutex_lock(&ping_mutex);
    }
  }
  mutex_unlock(&ping_mutex);
  mutex_unlock(&ping_cmd_mutex);

  if(err)
    return err;

  cli_printf(cli, "%d sent, %d received", count, received);
  if(received) {
    const int64_t avg = rtt_sum / received;
    cli_printf(cli, ", min/avg/max %d.%03d/%d.%03d/%d.%03d ms",
               (int)(rtt_min / 1000), (int)(rtt_min % 1000),
               (int)(avg / 1000), (int)(avg % 1000),
               (int)(rtt_max / 1000), (int)(rtt_max % 1000));
  }
  cli_printf(cli, "\n");
  return received ? 0 : ERR_TIMEOUT;
}

CLI_CMD_DEF_EXT("ping", cmd_ping, "<address> [count]",
                "Send ICMP echo requests");
