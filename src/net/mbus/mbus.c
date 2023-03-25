#include "mbus.h"
#include "util/crc32.h"

#include "net/pbuf.h"
#include "net/netif.h"
#include "net/socket.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mbus_rpc.h"
#include "mbus_dsig.h"

#include "mbus_seqpkt.h"


SLIST_HEAD(mbus_netif_list, mbus_netif);

//static struct socket_list mbus_sockets;

static struct mbus_netif_list mbus_netifs;

static uint8_t mbus_local_addr;

static uint32_t
mbus_crc32(struct pbuf *pb, uint32_t crc)
{
  for(; pb != NULL; pb = pb->pb_next)
    crc = crc32(crc, pb->pb_data + pb->pb_offset, pb->pb_buflen);

  return ~crc;
}


static pbuf_t *
mbus_output(pbuf_t *pb, uint8_t dst_addr)
{
  uint32_t crc = mbus_crc32(pb, 0);
  uint8_t *trailer = pbuf_append(pb, sizeof(uint32_t));
  trailer[0] = crc;
  trailer[1] = crc >> 8;
  trailer[2] = crc >> 16;
  trailer[3] = crc >> 24;

  mbus_netif_t *mni, *n;

  if(dst_addr < 32) {
    const uint32_t mask = 1 << dst_addr;

    SLIST_FOREACH(mni, &mbus_netifs, mni_global_link) {
      if(mask & mni->mni_active_hosts) {
        mni->mni_tx_bytes += pb->pb_pktlen;
        return mni->mni_output(mni, pb);
      }
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
      copy = mni->mni_output(mni, pb);
      if(copy != NULL)
        pbuf_free(copy);
    }
  }
  return pb;
}


pbuf_t *
mbus_output_unicast(pbuf_t *pb, uint8_t dst_addr, uint8_t type)
{
  pb = pbuf_prepend(pb, 2);

  uint8_t *hdr = pbuf_data(pb, 0);
  hdr[0] = dst_addr;
  hdr[1] = mbus_local_addr | (type << 5);
  return mbus_output(pb, dst_addr);
}


pbuf_t *
mbus_output_multicast(pbuf_t *pb, uint8_t group)
{
  pb = pbuf_prepend(pb, 1);
  uint8_t *hdr = pbuf_data(pb, 0);
  hdr[0] = group | 0x20;
  return mbus_output(pb, group);
}




struct pbuf *
mbus_local(mbus_netif_t *mni, pbuf_t *pb, uint8_t src_addr, uint8_t type)
{
  switch(type) {
  case 0:
    return mbus_seqpkt_input(pb, src_addr);

  default:
    // Unknown type
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

  if(pb->pb_pktlen < 2 || (pb = pbuf_pullup(pb, pb->pb_pktlen)) == NULL) {
    mni->mni_rx_runts++;
    return pb;
  }

  if(mbus_crc32(pb, 0)) {
    mni->mni_rx_crc_errors++;
    return pb;
  }

  const uint8_t *hdr = pbuf_cdata(pb, 0);
  const uint8_t dst_addr = hdr[0] & 0x3f;

  if(dst_addr & 0x20) {

    // Multicast


  } else {

    // Unicast

    const uint8_t src_addr = hdr[1] & 0x1f;
    const uint32_t src_mask = 1 << src_addr;

    if(!(mni->mni_active_hosts & src_mask)) {
      mbus_add_route(mni, src_mask);
    }

    if(dst_addr == mbus_local_addr) {
      const uint8_t type = hdr[1] >> 5;
      // Destined for us
      pb = pbuf_trim(pb, 4); // Drop CRC
      pb = pbuf_drop(pb, 2); // Drop header
      return mbus_local(mni, pb, src_addr, type);
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
  }
  return mbus_bcast(pb, mni);
}



void
mbus_print_info(mbus_netif_t *mni, struct stream *st)
{
  stprintf(st, "\tPacket interface:\n");
  stprintf(st, "\t\tRX %u bytes  %u packets\n",
           mni->mni_rx_bytes, mni->mni_rx_packets);
  stprintf(st, "\t\t   %u CRC  %u runts  %u bad opcode\n",
           mni->mni_rx_crc_errors, mni->mni_rx_runts,
           mni->mni_rx_unknown_opcode);
  stprintf(st, "\t\tTX %u bytes  %u sent  %u qdrops  %u failed\n",
           mni->mni_tx_bytes,  mni->mni_tx_packets,
           mni->mni_tx_qdrops, mni->mni_tx_fail);
  stprintf(st, "\tLocal address: %x\n", mbus_local_addr);
  stprintf(st, "\tActive hosts:");
  for(int i = 0; i < 32; i++) {
    if((1 << i) & mni->mni_active_hosts) {
      stprintf(st, "%x ", i);
    }
  }
  stprintf(st, "\n");
}



void
mbus_set_host_address(uint8_t addr)
{
  mbus_local_addr = addr;
}



void
mbus_netif_attach(mbus_netif_t *mni, const char *name,
                  const device_class_t *dc)
{
  mni->mni_ni.ni_input = mbus_input;

  netif_attach(&mni->mni_ni, name, dc);

  SLIST_INSERT_HEAD(&mbus_netifs, mni, mni_global_link);
}
