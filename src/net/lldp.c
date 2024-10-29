#include "ether.h"

#include <mios/hostname.h>
#include <mios/version.h>

#include <unistd.h>
#include <string.h>

#define LLDP_END          0
#define LLDP_CHASSIS_ID   1
#define LLDP_PORT_ID      2
#define LLDP_TTL          3
#define LLDP_SYSTEM_NAME  5
#define LLDP_SYSTEM_DESC  6

static const uint8_t lldp_dstaddr[6] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e};

static void *
make_tlv(pbuf_t *pb, uint8_t code, uint16_t len)
{
  if(len > 511)
    return NULL;

  uint8_t *dst = pbuf_append(pb, len + 2);
  if(dst == NULL)
    return NULL;
  dst[0] = (code << 1) | (len >> 8);
  dst[1] = len;
  return dst + 2;
}

static void *
make_tlv_with_subtype(pbuf_t *pb, uint8_t code, uint16_t len, uint8_t subtype)
{
  len++;
  uint8_t *r = make_tlv(pb, code, len);
  if(r == NULL)
    return NULL;
  r[0] = subtype;
  return r + 1;
}

static int
lldp_make(ether_netif_t *eni, pbuf_t *pb)
{
  uint8_t *data;

  if((data = make_tlv_with_subtype(pb, LLDP_CHASSIS_ID, 6, 4)) == NULL)
    return -1;
  memcpy(data, eni->eni_addr, 6);

  const char *ifname = eni->eni_ni.ni_dev.d_name;
  size_t ifnamelen = strlen(ifname);

  if((data = make_tlv_with_subtype(pb, LLDP_PORT_ID, ifnamelen, 7)) == NULL)
    return -1;
  memcpy(data, ifname, ifnamelen);

  if((data = make_tlv(pb, LLDP_TTL, 2)) == NULL)
    return -1;
  data[0] = 0;
  data[1] = 120;


  mutex_lock(&hostname_mutex);
  if(hostname[0]) {
    size_t hostnamelen = strlen(hostname);
    if((data = make_tlv(pb, LLDP_SYSTEM_NAME, hostnamelen)) == NULL) {
      mutex_unlock(&hostname_mutex);
      return -1;
    }
    memcpy(data, hostname, hostnamelen);
  }
  mutex_unlock(&hostname_mutex);

  const char *appname = mios_get_app_name();
  size_t appnamelen = strlen(appname);
  if(appnamelen > 0) {
    if((data = make_tlv(pb, LLDP_SYSTEM_DESC, appnamelen)) == NULL)
      return -1;
    memcpy(data, appname, appnamelen);
  }
  make_tlv(pb, LLDP_END, 0);
  return 0;
}

static void
lldp_send(ether_netif_t *eni)
{
  pbuf_t *pb = pbuf_make(16, 0);
  if(pb != NULL) {
    if(lldp_make(eni, pb)) {
      pbuf_free(pb);
      pb = NULL;
    }
  }

  if(pb != NULL) {
    pb = pbuf_prepend(pb, 14, 0, 0);
    if(pb != NULL) {
      uint8_t *eh = pbuf_data(pb, 0);
      memcpy(eh, lldp_dstaddr, 6);
      memcpy(eh + 6, eni->eni_addr, 6);
      eh[12] = 0x88;
      eh[13] = 0xcc;
      eni->eni_output(eni, pb, 0);
    }
  }

  int next = pb ? 30000000 : 5000000;
  net_timer_arm(&eni->eni_lldp_timer, clock_get() + next);
}



static void
lldp_timer_cb(void *opaque, uint64_t now)
{
  lldp_send(opaque);
}

void
lldp_status_change(ether_netif_t *eni)
{
  if(eni->eni_ni.ni_flags & NETIF_F_UP) {
    eni->eni_lldp_timer.t_cb = lldp_timer_cb;
    eni->eni_lldp_timer.t_opaque = eni;
    eni->eni_lldp_timer.t_name = "lldp";
    lldp_send(eni);
  } else {
    timer_disarm(&eni->eni_lldp_timer);
  }
}
