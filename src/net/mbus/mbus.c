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

#ifdef ENABLE_NET_PCS
#include "net/pcs_shell.h"
#endif

SLIST_HEAD(mbus_netif_list, mbus_netif);

struct mbus_netif_list mbus_netifs;

static struct socket_list mbus_sockets;

static uint32_t
mbus_crc32(struct pbuf *pb)
{
  uint32_t crc = 0;

  for(; pb != NULL; pb = pb->pb_next)
    crc = crc32(crc, pb->pb_data + pb->pb_offset, pb->pb_buflen);

  return ~crc;
}


pbuf_t *
mbus_output(mbus_netif_t *mni, struct pbuf *pb, uint8_t dst_addr)
{
  pb = pbuf_prepend(pb, MBUS_HDR_LEN);
  uint8_t *hdr = pbuf_data(pb, 0);

  const uint8_t addr = (dst_addr & 0xf) | (mni->mni_ni.ni_local_addr << 4);

  hdr[0] = addr;

  uint32_t crc = mbus_crc32(pb);
  uint8_t *trailer = pbuf_append(pb, sizeof(uint32_t));
  trailer[0] = crc;
  trailer[1] = crc >> 8;
  trailer[2] = crc >> 16;
  trailer[3] = crc >> 24;
  mni->mni_tx_bytes += pb->pb_pktlen;
  return mni->mni_output(mni, pb);
}


static pbuf_t *
mbus_handle_none(mbus_netif_t *mni, pbuf_t *pb, uint8_t src_addr)
{
  mni->mni_rx_unknown_opcode++;
  return pb;
}


static pbuf_t *
mbus_handle_ping(mbus_netif_t *mni, pbuf_t *pb, uint8_t src_addr)
{
  uint8_t *pkt = pbuf_data(pb, 0);
  pkt[0] = MBUS_OP_PONG;
  return mbus_output(mni, pb, src_addr);
}

typedef pbuf_t *(*ophandler_t)(struct mbus_netif *, pbuf_t *, uint8_t);

__attribute__((weak))
pbuf_t *
mbus_ota_upgrade(struct mbus_netif *ni, pbuf_t *pb, uint8_t src_addr)
{
  return pb;
}

static const ophandler_t ophandlers[16] = {
  [MBUS_OP_PING]        = mbus_handle_ping,
  [MBUS_OP_PONG]        = mbus_handle_none,
  [MBUS_OP_PUB_META]    = mbus_handle_none,
  [MBUS_OP_PUB_DATA]    = mbus_handle_none,
  [4]                   = mbus_handle_none,
  [5]                   = mbus_handle_none,
  [6]                   = mbus_handle_none,
  [MBUS_OP_DSIG_EMIT]   = mbus_dsig_input,
  [MBUS_OP_RPC_RESOLVE] = mbus_handle_rpc_resolve,
  [MBUS_OP_RPC_RESOLVE_REPLY] = mbus_handle_none,
  [MBUS_OP_RPC_INVOKE]  = mbus_handle_rpc_invoke,
  [MBUS_OP_RPC_ERR]     = mbus_handle_none,
  [MBUS_OP_RPC_REPLY]   = mbus_handle_none,
  [MBUS_OP_OTA_XFER]    = mbus_ota_upgrade,
  [14]                  = mbus_handle_none,
  [15]                  = mbus_handle_none,
};


struct pbuf *
mbus_local(mbus_netif_t *mni, pbuf_t *pb, uint8_t src_addr)
{
  const uint8_t *pkt = pbuf_cdata(pb, 0);
  if(pkt[0] & 0x80) {
    pb = pbuf_pullup(pb, pb->pb_pktlen);
    if(pb) {
#ifdef ENABLE_NET_PCS
      if(mni->mni_pcs != NULL)
        pcs_input(mni->mni_pcs, pbuf_cdata(pb, 0), pb->pb_pktlen, clock_get(),
                  src_addr);
#endif
    }
    return pb;
  }
  const uint8_t opcode = pkt[0] & 0xf;
  return ophandlers[opcode](mni, pb, src_addr);
}


static void
mbus_add_route(mbus_netif_t *act, uint16_t mask)
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

  if(mbus_crc32(pb)) {
    mni->mni_rx_crc_errors++;
    return pb;
  }

  if((pb = pbuf_pullup(pb, pb->pb_pktlen)) == NULL) {
    mni->mni_rx_runts++;
    return pb;
  }

  const uint8_t *hdr = pbuf_data(pb, 0);
  uint8_t addr = hdr[0];

  const uint8_t dst_addr = addr & 0xf;
  const uint8_t src_addr = addr >> 4;

  const uint16_t src_mask = 1 << src_addr;
  if(!(mni->mni_active_hosts & src_mask)) {
    mbus_add_route(mni, src_mask);
  }

  if(ni->ni_local_addr == dst_addr || dst_addr == 7) {
    // Destined for us or broadcast

    if(dst_addr == 0x7) {
      // Broadcast
      pb = mbus_bcast(pb, mni);
    }

    // Trim off CRC
    if((pb = pbuf_trim(pb, 4)) == NULL) {
      mni->mni_rx_runts++;
      return pb;
    }

    pb = pbuf_drop(pb, MBUS_HDR_LEN);

    return mbus_local(mni, pb, src_addr);
  }

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
  stprintf(st, "\tRX %u bytes  %u packets\n",
           mni->mni_rx_bytes, mni->mni_rx_packets);
  stprintf(st, "\t   %u CRC  %u runts  %u bad opcode\n",
           mni->mni_rx_crc_errors, mni->mni_rx_runts,
           mni->mni_rx_unknown_opcode);
  stprintf(st, "\tTX %u bytes  %u sent  %u qdrops  %u failed\n",
           mni->mni_tx_bytes,  mni->mni_tx_packets,
           mni->mni_tx_qdrops, mni->mni_tx_fail);
  stprintf(st, "\tActive hosts:");
  for(int i = 0; i < 16; i++) {
    if((1 << i) & mni->mni_active_hosts) {
      stprintf(st, "%x ", i);
    }
  }
  stprintf(st, "\n");
}


#ifdef ENABLE_NET_PCS


static int
mbus_pcs_accept(void *opaque, pcs_t *pcs, uint8_t channel)
{
  switch(channel) {
  case 0x80:
    return pcs_shell_create(pcs);
  }
  return -1;
}

static int64_t
mbus_pcs_wait_helper(cond_t *c, mutex_t *m, int64_t deadline)
{
  if(deadline == INT64_MAX) {
    cond_wait(c, m);
  } else {
    if(cond_wait_timeout(c, m, deadline, 0)) {}
  }
  return clock_get();
}


__attribute__((noreturn))
static void *
mbus_pcs_thread(void *arg)
{
  mbus_netif_t *mni = arg;
  const size_t mtu = mni->mni_ni.ni_mtu - MBUS_HDR_LEN - 4;

  while(1) {

    pbuf_t *pb = pbuf_make(MBUS_HDR_LEN, 1);

    pcs_poll_result_t ppr = pcs_wait(mni->mni_pcs, pbuf_data(pb, 0), mtu,
                                     clock_get(), mbus_pcs_wait_helper);

    pb->pb_pktlen = ppr.len;
    pb->pb_buflen = ppr.len;
    pb = mbus_output(mni, pb, ppr.addr);
    if(pb)
      pbuf_free(pb);
  }
}
#endif

void
mbus_netif_attach(mbus_netif_t *mni, const char *name,
                  const device_class_t *dc, uint8_t addr, int flags)
{
  mni->mni_ni.ni_local_addr = addr;
  mni->mni_ni.ni_input = mbus_input;

  netif_attach(&mni->mni_ni, name, dc);

  SLIST_INSERT_HEAD(&mbus_netifs, mni, mni_global_link);

#ifdef ENABLE_NET_PCS
  if(flags & MBUS_NETIF_ENABLE_PCS) {
    mni->mni_pcs = pcs_iface_create(mni, 64, mbus_pcs_accept);
    task_create(mbus_pcs_thread, mni, 384, "pcs", 0, 4);
  }
#endif
}


static error_t
mbus_control(socket_t *s, socket_ctl_t *sc)
{
  switch(sc->sc_op) {
  case SOCKET_CTL_ATTACH:
    s->s_header_size = MBUS_HDR_LEN;
    s->s_mtu = s->s_netif->ni_mtu - s->s_header_size - 4;
    LIST_INSERT_HEAD(&mbus_sockets, s, s_proto_link);
    return 0;

  case SOCKET_CTL_DETACH:
    LIST_REMOVE(s, s_proto_link);
    return 0;
  }
  return ERR_NOT_IMPLEMENTED;
}


static pbuf_t *
mbus_send(socket_t *s, pbuf_t *pb)
{
  mbus_netif_t *mni, *n;

  if(!(pb->pb_flags & PBUF_BCAST)) {
    const uint16_t m = 1 << s->s_remote_addr;

    SLIST_FOREACH(mni, &mbus_netifs, mni_global_link) {
      if(m & mni->mni_active_hosts) {
        return mbus_output(mni, pb, s->s_remote_addr);
      }
    }
  }

  for(mni = SLIST_FIRST(&mbus_netifs); mni != NULL; mni = n) {
    n = SLIST_NEXT(mni, mni_global_link);

    if(n == NULL)
      return mbus_output(mni, pb, 7);

    pbuf_t *copy = pbuf_copy(pb, 0);
    if(copy != NULL) {
      copy = mbus_output(mni, copy, 7);
      if(copy != NULL)
        pbuf_free(copy);
    }
  }
  return pb;
}

NET_SOCKET_PROTO_DEF(AF_MBUS, 0, mbus_control, mbus_send);
