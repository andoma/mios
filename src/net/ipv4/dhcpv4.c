#include <mios/mios.h>
#include <mios/cli.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/pbuf.h"
#include "dhcpv4.h"
#include "net/ether.h"
#include "net/net.h"

typedef struct dhcp_hdr {
  uint8_t op;
  uint8_t htype;
  uint8_t hlen;
  uint8_t hops;
  uint32_t xid;
  uint16_t secs;
  uint16_t flags;
  uint32_t ciaddr;
  uint32_t yiaddr;
  uint32_t siaddr;
  uint32_t giaddr;
  uint8_t chaddr[16];
  uint8_t sname[64];
  uint8_t file[128];
  uint8_t cookie[4];
} dhcp_hdr_t;

#define DHCP_STATE_SELECTING         0
#define DHCP_STATE_REQUESTING        1
#define DHCP_STATE_REQUESTING_RETRY  2
#define DHCP_STATE_BOUND             3
#define DHCP_STATE_RENEWING          4


#define DHCP_SUBNET_MASK               1
#define DHCP_GATEWAY                   3
#define DHCP_REQUESTED_IP_ADDRESS      50
#define DHCP_LEASE_TIME                51
#define DHCP_MESSAGE_TYPE              53
#define DHCP_SERVER_IDENTIFIER         54
#define DHCP_PARAMETER_REQUEST_LIST    55
#define DHCP_CLIENT_IDENTIFIER         61

#define DHCPDISCOVER       1
#define DHCPOFFER          2
#define DHCPREQUEST        3
#define DHCPDECLINE        4
#define DHCPACK            5

static void *
make_option(pbuf_t *pb, uint8_t code, uint8_t len)
{
  uint8_t *dst = pbuf_append(pb, len + 2);
  if(dst == NULL)
    return NULL;
  dst[0] = code;
  dst[1] = len;
  return dst + 2;
}


static void
append_option_copy(pbuf_t *pb, uint8_t code, uint8_t len, void *data)
{
  uint8_t *dst = make_option(pb, code, len);
  if(dst != NULL)
    memcpy(dst, data, len);
}

static void
append_option_u8(pbuf_t *pb, uint8_t code, uint8_t data)
{
  append_option_copy(pb, code, 1, &data);
}

static void
append_option_v4addr(pbuf_t *pb, uint8_t code, uint32_t addr)
{
  append_option_copy(pb, code, 4, &addr);
}

static void
append_end(pbuf_t *pb)
{
  uint8_t *dst = pbuf_append(pb, 1);
  if(dst == NULL)
    return;

  *dst = 0xff;
}


static void
append_client_identifier(pbuf_t *pb, const ether_netif_t *eni)
{
  uint8_t *ci = make_option(pb, DHCP_CLIENT_IDENTIFIER, 7);
  if(ci != NULL) {
    ci[0] = 1;
    memcpy(ci + 1, eni->eni_addr, 6);
  }
}

static void
append_parameter_request_list(pbuf_t *pb)
{
  uint8_t *ci = make_option(pb, DHCP_PARAMETER_REQUEST_LIST, 3);
  if(ci != NULL) {
    ci[0] = DHCP_SUBNET_MASK;
    ci[1] = DHCP_GATEWAY;
    ci[2] = DHCP_LEASE_TIME;
  }
}


static pbuf_t *
dhcpv4_make(ether_netif_t *eni)
{
  pbuf_t *pb = pbuf_make(16 + 20 + 8); // Make space for ether + ip + udp
  dhcp_hdr_t *dh = pbuf_append(pb, sizeof(dhcp_hdr_t));

  memset(dh, 0, sizeof(dhcp_hdr_t));
  dh->op = 1;
  dh->htype = 1;
  dh->hlen = 6;
  eni->eni_dhcp_xid = rand();
  dh->xid = eni->eni_dhcp_xid;
  dh->ciaddr = eni->eni_ni.ni_ipv4_addr;
  memcpy(dh->chaddr, eni->eni_addr, 6);
  dh->cookie[0] = 0x63;
  dh->cookie[1] = 0x82;
  dh->cookie[2] = 0x53;
  dh->cookie[3] = 0x63;
  return pb;
}


static void
dhcpv4_send(struct ether_netif *eni, pbuf_t *pb, uint32_t dst_addr,
           const char *why)
{
  size_t autopad = PBUF_DATA_SIZE - (pb->pb_buflen + pb->pb_offset);

  memset(pbuf_data(pb, pb->pb_buflen), 0, autopad);
  pb->pb_pktlen += autopad;
  pb->pb_buflen += autopad;

  size_t packet_len = pb->pb_pktlen;

  pb = pbuf_prepend(pb, 8);

  udp_hdr_t *udp = pbuf_data(pb, 0);
  udp->dst_port = htons(67);
  udp->src_port = htons(68);
  udp->length = htons(packet_len);
  udp->cksum = 0;

  pb = pbuf_prepend(pb, 20);
  ipv4_header_t *ip = pbuf_data(pb, 0);

  ip->ver_ihl = 0x45;
  ip->tos = 0;
  ip->total_length = htons(packet_len + 20 + 8);
  ip->id = rand();
  ip->fragment_info = 0;
  ip->ttl = 30;
  ip->proto = 17;
  ip->cksum = 0;
  ip->src_addr = eni->eni_ni.ni_ipv4_addr;
  ip->dst_addr = dst_addr;

  printf("DHCP: Sending to %Id (%s)\n", dst_addr, why);

  if(dst_addr == 0xffffffff) {
    ip->cksum = ipv4_cksum_pbuf(0, pb, 0, 20);
    eni->eni_ni.ni_ipv4_output(&eni->eni_ni, NULL, pb);
  } else {
    ipv4_output(pb);
  }
}




static void
dhcpv4_send_discover(struct ether_netif *eni)
{
  pbuf_t *pb = dhcpv4_make(eni);
  append_option_u8(pb, DHCP_MESSAGE_TYPE, DHCPDISCOVER);
  append_client_identifier(pb, eni);
  append_parameter_request_list(pb);
  append_end(pb);
  dhcpv4_send(eni, pb, 0xffffffff, "Discover");
}


static void
dhcpv4_send_request(struct ether_netif *eni, const char *why)
{
  pbuf_t *pb = dhcpv4_make(eni);
  append_option_u8(pb, DHCP_MESSAGE_TYPE, DHCPREQUEST);
  append_client_identifier(pb, eni);
  append_parameter_request_list(pb);

  if(!eni->eni_ni.ni_ipv4_addr) {
    // If have no address yet we are in SELECTING state
    append_option_v4addr(pb, DHCP_SERVER_IDENTIFIER, eni->eni_dhcp_server_ip);
    append_option_v4addr(pb, DHCP_REQUESTED_IP_ADDRESS,
                         eni->eni_dhcp_requested_ip);
  }

  append_end(pb);
  dhcpv4_send(eni, pb, eni->eni_ni.ni_ipv4_addr ?
              eni->eni_dhcp_server_ip : 0xffffffff, why);
}




void
dhcpv4_periodic(struct ether_netif *eni)
{
  const int64_t now = clock_get();

  switch(eni->eni_dhcp_state) {
  case DHCP_STATE_SELECTING:
    eni->eni_ni.ni_ipv4_addr = 0;
    eni->eni_dhcp_server_ip = 0;
    eni->eni_dhcp_requested_ip = 0;
    dhcpv4_send_discover(eni);
    break;
  case DHCP_STATE_BOUND:
    if(now < eni->eni_dhcp_timeout)
      break;
    // FALLTHRU
  case DHCP_STATE_REQUESTING:
    eni->eni_dhcp_state = DHCP_STATE_REQUESTING_RETRY;
    eni->eni_dhcp_timeout = now + 20000000ull;
    break;
  case DHCP_STATE_REQUESTING_RETRY:
    dhcpv4_send_request(eni, "request-retry");
    if(now > eni->eni_dhcp_timeout)
      eni->eni_dhcp_state = DHCP_STATE_SELECTING;
    break;
  }
}


typedef struct parsed_opts {
  uint16_t valid;
#define PO_MSGTYPE           0x1
#define PO_NETMASK           0x2
#define PO_GATEWAY           0x4
#define PO_LEASE_TIME        0x8
#define PO_SERVER_IDENTIFIER 0x10
  uint8_t msgtype;

  uint32_t gateway;
  uint32_t netmask;
  uint32_t lease_time;
  uint32_t server_identifer;
} parsed_opts_t;


static const struct {
  uint16_t flag;
  uint8_t type;
  uint8_t length;
  size_t offset;
} options[] = {
  { PO_MSGTYPE, DHCP_MESSAGE_TYPE, 1, offsetof(parsed_opts_t, msgtype) },
  { PO_NETMASK, DHCP_SUBNET_MASK, 4, offsetof(parsed_opts_t, netmask) },
  { PO_GATEWAY, DHCP_GATEWAY, 4, offsetof(parsed_opts_t, gateway) },
  { PO_LEASE_TIME, DHCP_LEASE_TIME, 4, offsetof(parsed_opts_t, lease_time) },
  { PO_SERVER_IDENTIFIER, DHCP_SERVER_IDENTIFIER, 4, offsetof(parsed_opts_t, server_identifer) }
};


#define REQUIRED_OPTIONS_FROM_SERVER \
  (PO_MSGTYPE | PO_NETMASK | PO_GATEWAY | PO_LEASE_TIME | PO_SERVER_IDENTIFIER)





pbuf_t *
parse_opts(pbuf_t *pb, struct parsed_opts *po)
{
  po->valid = 0;

  while(pb) {
    if((pb = pbuf_pullup(pb, 1)) == NULL)
      break;
    uint8_t type = *(const uint8_t *)pbuf_data(pb, 0);
    if(type == 0xff) // END
      break;
    pb = pbuf_drop(pb, 1);
    if(type == 0)
      continue;

    if((pb = pbuf_pullup(pb, 1)) == NULL)
      break;
    uint8_t length = *(const uint8_t *)pbuf_data(pb, 0);
    pb = pbuf_drop(pb, 1);
    if(length > pb->pb_pktlen)
      break;

    if((pb = pbuf_pullup(pb, length)) == NULL)
      break;
#if 0
    const uint8_t *dx = pbuf_data(pb, 0);
    printf("Option %3d:", type);
    for(int j = 0; j < length; j++)
      printf("%02x ", dx[j]);
    printf("\n");
#endif
    for(size_t i = 0; i < ARRAYSIZE(options); i++) {
      if(options[i].type == type && options[i].length == length) {

        po->valid |= options[i].flag;
        memcpy((void *)po + options[i].offset, pbuf_data(pb, 0), length);
        break;
      }
    }
    pb = pbuf_drop(pb, length);
  }
  return pb;
}



pbuf_t *
dhcpv4_input(struct netif *ni, pbuf_t *pb, uint32_t from)
{
  ether_netif_t *eni = (ether_netif_t *)ni;
  if((pb = pbuf_pullup(pb, sizeof(dhcp_hdr_t))) == NULL)
    return pb;

  const dhcp_hdr_t *dh = pbuf_data(pb, 0);
  if(dh->op != 2)
    return pb;

  if(dh->xid != eni->eni_dhcp_xid)
    return pb;

  const uint32_t yiaddr = dh->yiaddr;
  pbuf_drop(pb, sizeof(dhcp_hdr_t));

  parsed_opts_t po;
  if((pb = parse_opts(pb, &po)) == NULL)
    return pb;

  if((po.valid & REQUIRED_OPTIONS_FROM_SERVER) != REQUIRED_OPTIONS_FROM_SERVER)
    return pb;

  switch(po.msgtype) {
  case DHCPOFFER:
    if(eni->eni_dhcp_state != DHCP_STATE_SELECTING) {
      printf("DHCP: Got OFFER but we are not selecting\n");
      return pb;
    }

    printf("DHCPOFFER: %Id from %Id\n", yiaddr, from);
    eni->eni_dhcp_requested_ip = yiaddr;
    eni->eni_dhcp_server_ip = po.server_identifer;
    dhcpv4_send_request(eni, "got-offer");
    eni->eni_dhcp_state = DHCP_STATE_REQUESTING;
    break;

  case DHCPACK:

    if(eni->eni_dhcp_state == DHCP_STATE_REQUESTING ||
       eni->eni_dhcp_state == DHCP_STATE_REQUESTING_RETRY ||
       eni->eni_dhcp_state == DHCP_STATE_BOUND) {
      printf("DHCPACK: %Id from %Id\n", yiaddr, from);

      if(eni->eni_dhcp_requested_ip != yiaddr) {
        printf("  rejected, not requested ip\n");
        break;
      }

      if(eni->eni_dhcp_server_ip != from) {
        printf("  rejected, not correct source\n");
        break;
      }

      eni->eni_ni.ni_ipv4_addr = yiaddr;
      eni->eni_ni.ni_ipv4_prefixlen = 24; // XXX FIX
      eni->eni_dhcp_state = DHCP_STATE_BOUND;
      eni->eni_dhcp_timeout =
        clock_get() + 1000000ull * (ntohl(po.lease_time) / 2);
      break;
    }
    break;
  }
  return pb;
}





static const char *dhcp_state_str[] = {

  [DHCP_STATE_SELECTING] = "selecting",
  [DHCP_STATE_REQUESTING] = "requesting",
  [DHCP_STATE_REQUESTING_RETRY] = "requesting-retry",
  [DHCP_STATE_BOUND] = "bound",
  [DHCP_STATE_RENEWING] = "renewing"
};

static int
cmd_dhcp(cli_t *cli, int argc, char **argv)
{
  ether_netif_t *eni;

  mutex_lock(&netif_mutex);

  SLIST_FOREACH(eni, &ether_netifs, eni_global_link) {

    cli_printf(cli, "DHCP State: %s\n", dhcp_state_str[eni->eni_dhcp_state]);
    cli_printf(cli, "     Our address: %Id\n", eni->eni_dhcp_requested_ip);
    cli_printf(cli, "     Server: %Id\n", eni->eni_dhcp_server_ip);
    if(eni->eni_dhcp_timeout) {
      int delta = (eni->eni_dhcp_timeout - clock_get()) / 1000000;

      if(eni->eni_dhcp_state == DHCP_STATE_BOUND) {
        cli_printf(cli, "     Renew in: %d seconds\n", delta);
      } else if(eni->eni_dhcp_state != DHCP_STATE_SELECTING) {
        cli_printf(cli, "     Re-init in: %d seconds\n", delta);
      }
    }
  }
  mutex_unlock(&netif_mutex);
  return 0;
}


CLI_CMD_DEF("dhcp", cmd_dhcp);
