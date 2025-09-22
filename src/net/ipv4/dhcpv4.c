#include "dhcpv4.h"

#include "udp.h"
#include "ntp.h"

#include <mios/eventlog.h>
#include <mios/type_macros.h>
#include <mios/cli.h>
#include <mios/version.h>
#include <mios/ghook.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

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
#define DHCP_NTP_SERVER                42
#define DHCP_VENDOR_SPECIFIC_INFO      43
#define DHCP_REQUESTED_IP_ADDRESS      50
#define DHCP_LEASE_TIME                51
#define DHCP_MESSAGE_TYPE              53
#define DHCP_SERVER_IDENTIFIER         54
#define DHCP_PARAMETER_REQUEST_LIST    55
#define DHCP_VENDOR_CLASS_ID           60
#define DHCP_CLIENT_IDENTIFIER         61
#define DHCP_BOOT_FILE_NAME            67

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
append_option_copy(pbuf_t *pb, uint8_t code, uint8_t len, const void *data)
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


typedef struct {
  struct netif *ni;
  pbuf_t *vsi; // vendor specific info
  char *bootfile;
} dhcp_update_aux_t;

__attribute__((noreturn))
static void *
dhcpv4_update_thread(void *arg)
{
  dhcp_update_aux_t *dua = arg;
  pbuf_t *vsi = dua->vsi;
  const void *vsidata = vsi ? pbuf_data(vsi, 0) : NULL;
  size_t vsisize = vsi ? vsi->pb_buflen : 0;
  ghook_invoke(GHOOK_DHCP_UPDATE, dua->ni, vsidata, vsisize, dua->bootfile);
  pbuf_free(vsi);
  device_release(&dua->ni->ni_dev);
  free(dua->bootfile);
  free(dua);
  thread_exit(0);
}

static void
dhcpv4_update(netif_t *ni, pbuf_t **vsi, char **bootfile)
{
  dhcp_update_aux_t *dua = xalloc(sizeof(dhcp_update_aux_t),
                                  0, MEM_MAY_FAIL);
  if(dua == NULL)
    return;

  if(vsi != NULL) {
    dua->vsi = *vsi;
    *vsi = NULL;
  } else {
    dua->vsi = NULL;
  }

  if(bootfile != NULL) {
    dua->bootfile = *bootfile;
    *bootfile = NULL;
  } else {
    dua->bootfile = NULL;
  }

  dua->ni = ni;
  device_retain(&ni->ni_dev);

  thread_t *t = thread_create(dhcpv4_update_thread, dua,
                              512, "dhcpupdate", TASK_DETACHED, 1);
  if(t == NULL) {
    pbuf_free(dua->vsi);
    device_release(&ni->ni_dev);
    free(dua);
  }
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
  dh->ciaddr = eni->eni_ni.ni_ipv4_local_addr;
  memcpy(dh->chaddr, eni->eni_addr, 6);
  dh->cookie[0] = 0x63;
  dh->cookie[1] = 0x82;
  dh->cookie[2] = 0x53;
  dh->cookie[3] = 0x63;
  return pb;
}


static void
dhcpv4_send(struct ether_netif *eni, pbuf_t *pb, uint32_t dst_addr)
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

  udp_send(ni, pb, dst_addr, nh, 68, 67);
}



static void
append_default_options(pbuf_t *pb, struct ether_netif *eni)
{
  append_client_identifier(pb, eni);
  append_parameter_request_list(pb);

#ifdef DHCPV4_VCID
  append_option_copy(pb, DHCP_VENDOR_CLASS_ID, strlen(DHCPV4_VCID),
                     DHCPV4_VCID);
#endif
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
  dhcpv4_send(eni, pb, 0xffffffff);
}


static void
dhcpv4_send_request(struct ether_netif *eni)
{
  pbuf_t *pb = dhcpv4_make(eni);
  if(pb == NULL)
    return;
  append_option_u8(pb, DHCP_MESSAGE_TYPE, DHCPREQUEST);

  append_default_options(pb, eni);

  if(!eni->eni_ni.ni_ipv4_local_addr) {
    // If have no address yet we are in SELECTING state
    append_option_v4addr(pb, DHCP_SERVER_IDENTIFIER, eni->eni_dhcp_server_ip);
    append_option_v4addr(pb, DHCP_REQUESTED_IP_ADDRESS,
                         eni->eni_dhcp_requested_ip);
  }

  append_end(pb);
  dhcpv4_send(eni, pb, eni->eni_ni.ni_ipv4_local_addr ?
              eni->eni_dhcp_server_ip : 0xffffffff);
}


static void
dhcpv4_discover(ether_netif_t *eni)
{
  if(eni->eni_dhcp_state != DHCP_STATE_SELECTING) {
#ifdef DHCPV4_VCID
    const char *vcid = DHCPV4_VCID;
#else
    const char *vcid = "<unset>";
#endif
    evlog(LOG_INFO, "dhcp: selecting (vcid:%s)", vcid);
    eni->eni_dhcp_state = DHCP_STATE_SELECTING;
  }
  eni->eni_ni.ni_ipv4_local_addr = 0;
  eni->eni_dhcp_server_ip = 0;
  eni->eni_dhcp_requested_ip = 0;

  dhcpv4_send_discover(eni);

  net_timer_arm(&eni->eni_dhcp_timer, clock_get() + 500000);
}


static void
dhcpv4_request(ether_netif_t *eni)
{
  if(eni->eni_dhcp_state != DHCP_STATE_REQUESTING) {
    eni->eni_dhcp_state = DHCP_STATE_REQUESTING;
    eni->eni_dhcp_retries = 0;
  }

  dhcpv4_send_request(eni);
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
#define PO_NTP_SERVER        0x20
#define PO_VENDOR_INFO       0x40
  uint8_t msgtype;

  uint32_t gateway;
  uint32_t netmask;
  uint32_t lease_time;
  uint32_t server_identifer;
  uint32_t ntp_server;
  pbuf_t *vsi; // Vendor specific info
  char *bootfile;
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
  { PO_SERVER_IDENTIFIER, DHCP_SERVER_IDENTIFIER, 4, offsetof(parsed_opts_t, server_identifer) },
  { PO_NTP_SERVER, DHCP_NTP_SERVER, 4, offsetof(parsed_opts_t, ntp_server) }
};

#define REQUIRED_OPTIONS_FROM_SERVER \
  (PO_MSGTYPE | PO_NETMASK | PO_GATEWAY | PO_LEASE_TIME | PO_SERVER_IDENTIFIER)


pbuf_t *
parse_opts(pbuf_t *pb, struct parsed_opts *po)
{
  po->valid = 0;
  po->vsi = NULL;
  po->bootfile = NULL;

  while(pb) {
    if(pbuf_pullup(pb, 1))
      break;
    uint8_t type = *(const uint8_t *)pbuf_data(pb, 0);
    if(type == 0xff) // END
      break;
    pb = pbuf_drop(pb, 1, 0);
    if(type == 0)
      continue;

    if(pbuf_pullup(pb, 1))
      break;
    uint8_t length = *(const uint8_t *)pbuf_data(pb, 0);
    pb = pbuf_drop(pb, 1, 0);
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

    if(type == DHCP_VENDOR_SPECIFIC_INFO && po->vsi == NULL) {
      po->vsi = pbuf_make(0, 0);
      if(po->vsi != NULL) {
        memcpy(pbuf_append(po->vsi, length), pbuf_data(pb, 0), length);
      }
    }

    if(type == DHCP_BOOT_FILE_NAME && po->bootfile == NULL) {
      po->bootfile = xalloc(length + 1, 0, MEM_MAY_FAIL);
      if(po->bootfile != NULL) {
        memcpy(po->bootfile, pbuf_data(pb, 0), length);
        po->bootfile[length] = 0;
      }
    }

    for(size_t i = 0; i < ARRAYSIZE(options); i++) {
      if(options[i].type == type && options[i].length == length) {

        po->valid |= options[i].flag;
        memcpy((void *)po + options[i].offset, pbuf_data(pb, 0), length);
        break;
      }
    }
    pb = pbuf_drop(pb, length, 0);
  }
  return pb;
}



static pbuf_t *
dhcpv4_input(struct netif *ni, pbuf_t *pb, size_t udp_offset)
{
  ether_netif_t *eni = (ether_netif_t *)ni;

  const ipv4_header_t *ip = pbuf_data(pb, 0);
  const uint32_t from = ip->src_addr;

  pb = pbuf_drop(pb, udp_offset + 8, 0);

  if(pbuf_pullup(pb, sizeof(dhcp_hdr_t)))
    return pb;

  const dhcp_hdr_t *dh = pbuf_data(pb, 0);
  if(dh->op != 2)
    return pb;

  if(dh->xid != eni->eni_dhcp_xid)
    return pb;

  const uint32_t yiaddr = dh->yiaddr;
  pb = pbuf_drop(pb, sizeof(dhcp_hdr_t), 0);

  parsed_opts_t po;
  if((pb = parse_opts(pb, &po)) != NULL &&
     (po.valid & REQUIRED_OPTIONS_FROM_SERVER) ==
     REQUIRED_OPTIONS_FROM_SERVER) {

    switch(po.msgtype) {
    case DHCPOFFER:
      if(eni->eni_dhcp_state != DHCP_STATE_SELECTING) {
        evlog(LOG_INFO, "dhcp: Got OFFER but we are not selecting");
        break;
      }

      evlog(LOG_INFO, "dhcp: OFFER %Id from %Id", yiaddr, from);
      eni->eni_dhcp_requested_ip = yiaddr;
      eni->eni_dhcp_server_ip = po.server_identifer;

      eni->eni_dhcp_retries = 0;
      dhcpv4_request(eni);
      break;

    case DHCPACK:

      if(eni->eni_dhcp_state != DHCP_STATE_REQUESTING &&
         eni->eni_dhcp_state != DHCP_STATE_BOUND)
        break;

      if(eni->eni_ni.ni_ipv4_local_addr != yiaddr) {
        // Only log when something changes from our bound address
        evlog(LOG_INFO, "dhcp: ACK %Id from %Id", yiaddr, from);
      }

      if(eni->eni_dhcp_requested_ip != yiaddr) {
        evlog(LOG_INFO, "dhcp: rejected, not requested ip");
        break;
      }

      if(eni->eni_dhcp_server_ip != from) {
        evlog(LOG_INFO, "dhcp: rejected, not correct source");
        break;
      }

      eni->eni_ni.ni_ipv4_local_addr = yiaddr;
      eni->eni_ni.ni_ipv4_local_prefixlen =
        33 - __builtin_ffs(ntohl(po.netmask));
      eni->eni_dhcp_state = DHCP_STATE_BOUND;
      dhcpv4_update(&eni->eni_ni, &po.vsi, &po.bootfile);
      net_timer_arm(&eni->eni_dhcp_timer,
                    clock_get() + 1000000ull * (ntohl(po.lease_time) / 2));

      if(po.valid & PO_NTP_SERVER) {
        ntp_set_server(po.ntp_server);
      }
      break;
    }
  }
  pbuf_free(po.vsi);
  free(po.bootfile);
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
      dhcpv4_request(eni);
    break;
  case DHCP_STATE_BOUND:
    dhcpv4_request(eni);
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
  dhcpv4_update(&eni->eni_ni, NULL, NULL);
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
    dhcpv4_request(eni);
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

