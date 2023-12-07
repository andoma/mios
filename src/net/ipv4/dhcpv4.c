#include "dhcpv4.h"

#include "udp.h"

#include <mios/eventlog.h>
#include <mios/mios.h>
#include <mios/cli.h>
#include <mios/version.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/pbuf.h"
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

#define DHCP_STATE_IDLE              0
#define DHCP_STATE_SELECTING         1
#define DHCP_STATE_REQUESTING        2
#define DHCP_STATE_BOUND             3


#define DHCP_SUBNET_MASK               1
#define DHCP_GATEWAY                   3
#define DHCP_REQUESTED_IP_ADDRESS      50
#define DHCP_LEASE_TIME                51
#define DHCP_MESSAGE_TYPE              53
#define DHCP_SERVER_IDENTIFIER         54
#define DHCP_PARAMETER_REQUEST_LIST    55
#define DHCP_VENDOR_CLASS_ID           60
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


static void
dhcpv4_update(netif_t *ni)
{
  extern unsigned long _dhcpv4update_array_begin;
  extern unsigned long _dhcpv4update_array_end;

  const dhcpv4_update_t *update = (void *)&_dhcpv4update_array_begin;
  for(; update != (const void *)&_dhcpv4update_array_end; update++)
    (*update)(ni);
}

static pbuf_t *
dhcpv4_make(ether_netif_t *eni)
{
  pbuf_t *pb = pbuf_make(16 + 20 + 8, 0); // Make space for ether + ip + udp
  if(pb == NULL)
    return NULL;
  dhcp_hdr_t *dh = pbuf_append(pb, sizeof(dhcp_hdr_t));

  memset(dh, 0, sizeof(dhcp_hdr_t));
  dh->op = 1;
  dh->htype = 1;
  dh->hlen = 6;
  eni->eni_dhcp_xid = rand();
  dh->xid = eni->eni_dhcp_xid;
  dh->ciaddr = eni->eni_ni.ni_local_addr;
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
  netif_t *ni = &eni->eni_ni;
  nexthop_t *nh = NULL;

  if(dst_addr != 0xffffffff) {
    nh = ipv4_nexthop_resolve(dst_addr);
    if(nh == NULL) {
      pbuf_free(pb);
      return;
    }
    ni = nh->nh_netif;
  }

  const size_t autopad = PBUF_DATA_SIZE - (pb->pb_buflen + pb->pb_offset);

  memset(pbuf_data(pb, pb->pb_buflen), 0, autopad);
  pb->pb_pktlen += autopad;
  pb->pb_buflen += autopad;

  pb = pbuf_prepend(pb, sizeof(udp_hdr_t), 1, sizeof(ipv4_header_t));

  udp_hdr_t *udp = pbuf_data(pb, 0);
  udp->dst_port = htons(67);
  udp->src_port = htons(68);
  udp->length = htons(pb->pb_pktlen);

  udp->cksum = 0;

  if(!(ni->ni_flags & NETIF_F_TX_IPV4_CKSUM_OFFLOAD)) {
    udp->cksum =
      ipv4_cksum_pbuf(ipv4_cksum_pseudo(ni->ni_local_addr, dst_addr,
                                        IPPROTO_UDP, pb->pb_pktlen),
                      pb, 0, pb->pb_pktlen);
  }

  pb = pbuf_prepend(pb, sizeof(ipv4_header_t), 1, 0);
  ipv4_header_t *ip = pbuf_data(pb, 0);

  ip->ver_ihl = 0x45;
  ip->tos = 0;
  ip->total_length = htons(pb->pb_pktlen);
  ip->id = rand();
  ip->fragment_info = 0;
  ip->ttl = 255;
  ip->proto = IPPROTO_UDP;
  ip->src_addr = ni->ni_local_addr;
  ip->dst_addr = dst_addr;

  ip->cksum = 0;
  if(!(ni->ni_flags & NETIF_F_TX_IPV4_CKSUM_OFFLOAD)) {
    ip->cksum = ipv4_cksum_pbuf(0, pb, 0, sizeof(ipv4_header_t));
  }

  ni->ni_output(ni, nh, pb);
}



static void
append_default_options(pbuf_t *pb, struct ether_netif *eni)
{
  append_client_identifier(pb, eni);
  append_parameter_request_list(pb);

  const char *mios_ver = mios_get_version();
  const char *app_name = mios_get_app_name();
  const char *app_ver = mios_get_app_version();

  if(*app_ver == 0)
    app_ver = "none";

  if(*app_name == 0)
    app_name = "none";

  const size_t mios_ver_len = strlen(mios_ver);
  const size_t app_name_len = strlen(app_name);
  const size_t app_ver_len = strlen(app_ver);

  const size_t vcidlen = strlen("mios") + 1 + mios_ver_len + 1 +
    app_name_len + 1 + app_ver_len;

  if(vcidlen > 255)
    return;

  uint8_t *vcid = pbuf_append(pb, vcidlen + 2);
  if(vcid == NULL)
    return;

  vcid[0] = DHCP_VENDOR_CLASS_ID;
  vcid[1] = vcidlen;
  vcid += 2;

  memcpy(vcid, "mios:", 5);
  vcid += 5;

  memcpy(vcid, mios_ver, mios_ver_len);
  vcid += mios_ver_len;
  *vcid++ = ':';

  memcpy(vcid, app_name, app_name_len);
  vcid += app_name_len;
  *vcid++ = ':';

  memcpy(vcid, app_ver, app_ver_len);
}


static void
dhcpv4_send_discover(struct ether_netif *eni)
{
  pbuf_t *pb = dhcpv4_make(eni);
  if(pb == NULL)
    return;
  append_option_u8(pb, DHCP_MESSAGE_TYPE, DHCPDISCOVER);

  append_default_options(pb, eni);

  append_end(pb);
  dhcpv4_send(eni, pb, 0xffffffff, "Discover");
}


static void
dhcpv4_send_request(struct ether_netif *eni, const char *why)
{
  pbuf_t *pb = dhcpv4_make(eni);
  if(pb == NULL)
    return;
  append_option_u8(pb, DHCP_MESSAGE_TYPE, DHCPREQUEST);

  append_default_options(pb, eni);

  if(!eni->eni_ni.ni_local_addr) {
    // If have no address yet we are in SELECTING state
    append_option_v4addr(pb, DHCP_SERVER_IDENTIFIER, eni->eni_dhcp_server_ip);
    append_option_v4addr(pb, DHCP_REQUESTED_IP_ADDRESS,
                         eni->eni_dhcp_requested_ip);
  }

  append_end(pb);
  dhcpv4_send(eni, pb, eni->eni_ni.ni_local_addr ?
              eni->eni_dhcp_server_ip : 0xffffffff, why);
}


static void
dhcpv4_discover(ether_netif_t *eni)
{
  if(eni->eni_dhcp_state != DHCP_STATE_SELECTING) {
    evlog(LOG_INFO, "dhcp: selecting");
    eni->eni_dhcp_state = DHCP_STATE_SELECTING;
  }
  eni->eni_ni.ni_local_addr = 0;
  eni->eni_dhcp_server_ip = 0;
  eni->eni_dhcp_requested_ip = 0;

  dhcpv4_send_discover(eni);

  net_timer_arm(&eni->eni_dhcp_timer, clock_get() + 500000);
}


static void
dhcpv4_request(ether_netif_t *eni, const char *reason)
{
  if(eni->eni_dhcp_state != DHCP_STATE_REQUESTING) {
    eni->eni_dhcp_state = DHCP_STATE_REQUESTING;
    evlog(LOG_INFO, "dhcp: requesting");
    eni->eni_dhcp_retries = 0;
  }

  dhcpv4_send_request(eni, reason);
  eni->eni_dhcp_retries++;
  net_timer_arm(&eni->eni_dhcp_timer,
                clock_get() + 500000 * eni->eni_dhcp_retries);
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
    if(pbuf_pullup(pb, 1))
      break;
    uint8_t type = *(const uint8_t *)pbuf_data(pb, 0);
    if(type == 0xff) // END
      break;
    pb = pbuf_drop(pb, 1);
    if(type == 0)
      continue;

    if(pbuf_pullup(pb, 1))
      break;
    uint8_t length = *(const uint8_t *)pbuf_data(pb, 0);
    pb = pbuf_drop(pb, 1);
    if(length > pb->pb_pktlen)
      break;

    if(pbuf_pullup(pb, length))
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



static pbuf_t *
dhcpv4_input(struct netif *ni, pbuf_t *pb, size_t udp_offset)
{
  ether_netif_t *eni = (ether_netif_t *)ni;

  const ipv4_header_t *ip = pbuf_data(pb, 0);
  const uint32_t from = ip->src_addr;

  pb = pbuf_drop(pb, udp_offset + 8);

  if(pbuf_pullup(pb, sizeof(dhcp_hdr_t)))
    return pb;

  const dhcp_hdr_t *dh = pbuf_data(pb, 0);
  if(dh->op != 2)
    return pb;

  if(dh->xid != eni->eni_dhcp_xid)
    return pb;

  const uint32_t yiaddr = dh->yiaddr;
  pb = pbuf_drop(pb, sizeof(dhcp_hdr_t));

  parsed_opts_t po;
  if((pb = parse_opts(pb, &po)) == NULL)
    return pb;

  if((po.valid & REQUIRED_OPTIONS_FROM_SERVER) != REQUIRED_OPTIONS_FROM_SERVER)
    return pb;

  switch(po.msgtype) {
  case DHCPOFFER:
    if(eni->eni_dhcp_state != DHCP_STATE_SELECTING) {
      evlog(LOG_INFO, "dhcp: Got OFFER but we are not selecting");
      return pb;
    }

    evlog(LOG_INFO, "dhcp: OFFER %Id from %Id", yiaddr, from);
    eni->eni_dhcp_requested_ip = yiaddr;
    eni->eni_dhcp_server_ip = po.server_identifer;

    eni->eni_dhcp_retries = 0;
    dhcpv4_request(eni, "got-offer");
    break;

  case DHCPACK:

    if(eni->eni_dhcp_state == DHCP_STATE_REQUESTING ||
       eni->eni_dhcp_state == DHCP_STATE_BOUND) {
      evlog(LOG_INFO, "dhcp: ACK %Id from %Id", yiaddr, from);

      if(eni->eni_dhcp_requested_ip != yiaddr) {
        evlog(LOG_INFO, "dhcp: rejected, not requested ip");
        break;
      }

      if(eni->eni_dhcp_server_ip != from) {
        evlog(LOG_INFO, "dhcp: rejected, not correct source");
        break;
      }

      eni->eni_ni.ni_local_addr = yiaddr;
      eni->eni_ni.ni_local_prefixlen = 33 - __builtin_ffs(ntohl(po.netmask));
      eni->eni_dhcp_state = DHCP_STATE_BOUND;
      dhcpv4_update(&eni->eni_ni);
      net_timer_arm(&eni->eni_dhcp_timer,
                    clock_get() + 1000000ull * (ntohl(po.lease_time) / 2));
      break;
    }
    break;
  }
  return pb;
}

UDP_INPUT(dhcpv4_input, 68);

static void
dhcp_timer_cb(void *opaque, uint64_t now)
{
  ether_netif_t *eni = opaque;
  switch(eni->eni_dhcp_state) {
  case DHCP_STATE_SELECTING:
    dhcpv4_discover(eni);
    break;
  case DHCP_STATE_REQUESTING:
    if(eni->eni_dhcp_retries >= 5)
      dhcpv4_discover(eni);
    else
      dhcpv4_request(eni, "retry");
    break;
  case DHCP_STATE_BOUND:
    dhcpv4_request(eni, "renewal");
    break;
  }
}

static void
dhcpv4_init(ether_netif_t *eni)
{
  eni->eni_dhcp_timer.t_cb = dhcp_timer_cb;
  eni->eni_dhcp_timer.t_opaque = eni;
  eni->eni_dhcp_timer.t_name = "dhcp";
}


void
dhcpv4_status_change(ether_netif_t *eni)
{
  dhcpv4_update(&eni->eni_ni);
  if(!(eni->eni_ni.ni_flags & NETIF_F_UP))
    return; // Interface is not up, don't do anything just now

  switch(eni->eni_dhcp_state) {
  case DHCP_STATE_IDLE:
    dhcpv4_init(eni);
  case DHCP_STATE_SELECTING:
    dhcpv4_discover(eni);
    break;
  case DHCP_STATE_REQUESTING:
  case DHCP_STATE_BOUND:
    dhcpv4_request(eni, "link-up");
    break;
  }

}



static const char *dhcp_state_str =
  "idle\0"
  "selecting\0"
  "requesting\0"
  "bound\0";

void
dhcpv4_print(ether_netif_t *eni, struct stream *st)
{
  stprintf(st, "\tDHCP State: %s\n",
           strtbl(dhcp_state_str, eni->eni_dhcp_state));
  stprintf(st, "\t\tOur address: %Id\n", eni->eni_dhcp_requested_ip);
  stprintf(st, "\t\tServer: %Id\n", eni->eni_dhcp_server_ip);
  if(eni->eni_dhcp_state) {
    int delta = (eni->eni_dhcp_timer.t_expire - clock_get()) / 1000000;
    if(eni->eni_dhcp_state == DHCP_STATE_BOUND) {
      stprintf(st, "\t\tRenew in: %d seconds\n", delta);
    } else if(eni->eni_dhcp_state != DHCP_STATE_SELECTING) {
      stprintf(st, "\t\tRe-init in: %d seconds\n", delta);
    }
  }
}

