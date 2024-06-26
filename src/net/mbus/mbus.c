#include "mbus.h"
#include "util/crc32.h"

#include <mios/eventlog.h>
#include <mios/mios.h>

#include "net/pbuf.h"
#include "net/netif.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mbus_rpc.h"
#include "mbus_flow.h"

#include "mbus_seqpkt.h"

#ifdef ENABLE_NET_DSIG
#include "net/dsig.h"
#endif


SLIST_HEAD(mbus_netif_list, mbus_netif);

static struct mbus_netif_list mbus_netifs;

uint8_t mbus_local_addr;

static struct mbus_flow_list mbus_flows;

uint32_t
mbus_crc32(struct pbuf *pb, uint32_t crc)
{
  for(; pb != NULL; pb = pb->pb_next)
    crc = crc32(crc, pb->pb_data + pb->pb_offset, pb->pb_buflen);

  return ~crc;
}

void
mbus_append_crc(struct pbuf *pb)
{
  uint32_t crc = mbus_crc32(pb, 0);
  uint8_t *trailer = pbuf_append(pb, sizeof(uint32_t));
  trailer[0] = crc;
  trailer[1] = crc >> 8;
  trailer[2] = crc >> 16;
  trailer[3] = crc >> 24;
}


static pbuf_t *
mbus_output_unicast(pbuf_t *pb)
{
  const uint8_t *hdr = pbuf_cdata(pb, 0);
  const int dst_addr = hdr[0];

  mbus_append_crc(pb);

  mbus_netif_t *mni, *n;
  const uint32_t mask = 1 << dst_addr;

  SLIST_FOREACH(mni, &mbus_netifs, mni_global_link) {
    if(mask & mni->mni_active_hosts) {
      mni->mni_tx_bytes += pb->pb_pktlen;
      return mni->mni_output(mni, pb);
    }
  }

  for(mni = SLIST_FIRST(&mbus_netifs); mni != NULL; mni = n) {
    n = SLIST_NEXT(mni, mni_global_link);

    if(n == NULL) {
      mni->mni_tx_bytes += pb->pb_pktlen;
      return mni->mni_output(mni, pb);
    }

    pbuf_t *copy = pbuf_copy(pb, 0);
    if(copy != NULL) {

      mni->mni_tx_bytes += pb->pb_pktlen;
      copy = mni->mni_output(mni, copy);
      if(copy != NULL)
        pbuf_free(copy);
    }
  }
  return pb;
}


pbuf_t *
mbus_output_flow(pbuf_t *pb, const mbus_flow_t *mf)
{
  pb = pbuf_prepend(pb, 3, 0, 0);
  if(pb == NULL)
    return NULL;
  uint8_t *hdr = pbuf_data(pb, 0);
  hdr[0] = mf->mf_remote_addr;
  hdr[1] = mbus_local_addr | ((mf->mf_flow >> 3) & 0x60);
  hdr[2] = mf->mf_flow;
  return mbus_output_unicast(pb);
}


static pbuf_t *
mbus_ping(pbuf_t *pb, uint8_t remote_addr, uint16_t flow)
{
  pbuf_reset(pb, 0, 0);
  uint8_t *pkt = pbuf_append(pb, 3);
  pkt[0] = remote_addr;
  pkt[1] = ((flow >> 3) & 0x60) | mbus_local_addr;
  pkt[2] = flow;
  return mbus_output_unicast(pb);
}


static mutex_t mbus_send_mutex = MUTEX_INITIALIZER("mbussend");

static struct pbuf_queue mbus_send_queue =
  STAILQ_HEAD_INITIALIZER(mbus_send_queue);

static void
mbus_send_cb(net_task_t *nt, uint32_t signals)
{
  while(1) {
    mutex_lock(&mbus_send_mutex);
    pbuf_t *pb = pbuf_splice(&mbus_send_queue);
    mutex_unlock(&mbus_send_mutex);
    if(pb == NULL)
      break;
    pb = mbus_output_unicast(pb);
    if(pb != NULL)
      pbuf_free(pb);
  }
}

static net_task_t mbus_send_task = { mbus_send_cb };

void
mbus_send(pbuf_t *pb)
{
  mutex_lock(&mbus_send_mutex);
  int empty = !STAILQ_FIRST(&mbus_send_queue);
  STAILQ_INSERT_TAIL(&mbus_send_queue, pb, pb_link);
  mutex_unlock(&mbus_send_mutex);
  if(empty) {
    net_task_raise(&mbus_send_task, 1);
  }
}


void
mbus_flow_insert(mbus_flow_t *mf)
{
  LIST_INSERT_HEAD(&mbus_flows, mf, mf_link);
}

void
mbus_flow_remove(mbus_flow_t *mf)
{
  if(mf->mf_input) {
    LIST_REMOVE(mf, mf_link);
    mf->mf_input = NULL;
  }
}


mbus_flow_t *
mbus_flow_find(uint8_t remote_addr, uint16_t flow)
{
  mbus_flow_t *mf;
  LIST_FOREACH(mf, &mbus_flows, mf_link) {
    if(mf->mf_flow == flow && mf->mf_remote_addr == remote_addr) {
      return mf;
    }
  }
  return NULL;
}


struct pbuf *
mbus_local(mbus_netif_t *mni, pbuf_t *pb)
{
  if(pb->pb_buflen < 3)
    return pb;

  const uint8_t *pkt = pbuf_cdata(pb, 0);
  const uint16_t flow = pkt[2] | ((pkt[1] << 3) & 0x300);
  const uint8_t src_addr = pkt[1] & 0x1f;
  const int init = pkt[1] & 0x80;

  if(init) {
    if(pb->pb_buflen < 4)
      return pb;

    const uint8_t type = pkt[3];

    switch(type) {
    case 0:
      return mbus_ping(pb, src_addr, flow);
    case 1:
      return mbus_seqpkt_accept(pbuf_drop(pb, 4), src_addr, flow);
#ifdef ENABLE_RPC
    case 2:
      return mbus_rpc_dispatch(pbuf_drop(pb, 4), src_addr, flow);
#endif
    default:
      return pb;
    }
  } else {
    mbus_flow_t *mf = mbus_flow_find(src_addr, flow);
    if(mf != NULL)
      return mf->mf_input(mf, pbuf_drop(pb, 3));

    return pb;
  }
}


static void
mbus_add_route(mbus_netif_t *act, uint32_t mask)
{
  mbus_netif_t *mni;
  SLIST_FOREACH(mni, &mbus_netifs, mni_global_link) {
    mni->mni_active_hosts &= ~mask;
  }
  act->mni_active_hosts |= mask;
}


static pbuf_t *
mbus_bcast(pbuf_t *pb, mbus_netif_t *src)
{
  mbus_netif_t *mni;
  SLIST_FOREACH(mni, &mbus_netifs, mni_global_link) {
    if(mni == src)
      continue;

    pbuf_t *copy = pbuf_copy(pb, 0);
    if(copy == NULL)
      continue;
    mni->mni_tx_bytes += copy->pb_pktlen;
    copy = mni->mni_output(mni, copy);
    if(copy != NULL)
      pbuf_free(copy);
  }
  return pb;
}


struct pbuf *
mbus_input(struct netif *ni, struct pbuf *pb)
{
  mbus_netif_t *mni = (mbus_netif_t *)ni;

  mni->mni_rx_packets++;
  mni->mni_rx_bytes += pb->pb_pktlen;

  if(pb->pb_pktlen < 2) {
    mni->mni_rx_runts++;
    return pb;
  }

  if(pbuf_pullup(pb, pb->pb_pktlen)) {
    panic("pullup failed");
  }

  if(mbus_crc32(pb, 0)) {
    mni->mni_rx_crc_errors++;
    return pb;
  }

  const uint8_t *hdr = pbuf_cdata(pb, 0);
  const uint32_t dst_addr = hdr[0] & 0x3f;

  if(dst_addr & 0x20) {
#ifdef ENABLE_NET_DSIG
    const uint16_t group = ((dst_addr & 0x1f) << 8) | hdr[1];

    pb = pbuf_drop(pb, 2); // Drop header
    pbuf_trim(pb, 4);      // Drop CRC

    return dsig_input(group, pb, ni);
#else
    return pb;
#endif
  }

  // Unicast

  const uint8_t src_addr = hdr[1] & 0x1f;
  const uint32_t src_mask = 1 << src_addr;

  if(!(mni->mni_active_hosts & src_mask)) {
    mbus_add_route(mni, src_mask);
  }

  if(dst_addr == mbus_local_addr) {
    pbuf_trim(pb, 4); // Drop CRC
    return mbus_local(mni, pb);
  }

  // Forward if we know about the host on a different interface
  mbus_netif_t *n;
  SLIST_FOREACH(n, &mbus_netifs, mni_global_link) {
    if(mni == n)
      continue;
    if(n->mni_active_hosts & (1 << dst_addr)) {
      n->mni_tx_bytes += pb->pb_pktlen;
      return n->mni_output(n, pb);
    }
  }
  return mbus_bcast(pb, mni);
}



void
mbus_print_info(mbus_netif_t *mni, struct stream *st)
{
  stprintf(st, "\tRX Packets:\n");
  stprintf(st, "\t\tBytes:%u  Packets:%u  ",
           mni->mni_rx_bytes, mni->mni_rx_packets);
  stprintf(st, "CRC:%u  Runts:%u\n",
           mni->mni_rx_crc_errors, mni->mni_rx_runts);
  stprintf(st, "\tTX Packets:\n");

  stprintf(st, "\t\tBytes:%u  Sent:%u  Qdrops:%u  Failed:%u\n",
           mni->mni_tx_bytes,  mni->mni_tx_packets,
           mni->mni_tx_qdrops, mni->mni_tx_fail);
  stprintf(st, "\tLocal address: %d\n", mbus_local_addr);
  stprintf(st, "\tActive hosts:");
  for(int i = 0; i < 32; i++) {
    if((1 << i) & mni->mni_active_hosts) {
      stprintf(st, "%d ", i);
    }
  }
  stprintf(st, "\n");
}



void
mbus_set_host_address(uint8_t addr)
{
  mbus_local_addr = addr;
}


#ifdef ENABLE_NET_DSIG

static pbuf_t *
mbus_dsig_output(struct netif *ni, pbuf_t *pb, uint32_t group, uint32_t flags)
{
  if(group > 8191)
    return pb;

  pb = pbuf_prepend(pb, 2, 0, 0);
  if(pb == NULL)
    return pb;

  uint8_t *hdr = pbuf_data(pb, 0);
  hdr[0] = 0x20 | (group >> 8);
  hdr[1] = group;

  mbus_append_crc(pb);

  mbus_netif_t *mni = (mbus_netif_t *)ni;
  return mni->mni_output(mni, pb);
}

#endif

void
mbus_netif_attach(mbus_netif_t *mni, const char *name,
                  const device_class_t *dc)
{
  mni->mni_ni.ni_input = mbus_input;
  mni->mni_ni.ni_mtu = 64;
#ifdef ENABLE_NET_DSIG
  mni->mni_ni.ni_dsig_output = mbus_dsig_output;
#endif
  netif_attach(&mni->mni_ni, name, dc);

  SLIST_INSERT_HEAD(&mbus_netifs, mni, mni_global_link);
}


void
mbus_netif_detach(mbus_netif_t *mni)
{
  SLIST_REMOVE(&mbus_netifs, mni, mbus_netif, mni_global_link);
  netif_detach(&mni->mni_ni); // mni may be free'd after this
}
