#pragma once

#include "net/netif.h"

#ifdef ENABLE_NET_PCS
#include <mios/timer.h>
#include "net/pcs/pcs.h"
#endif

typedef struct mbus_netif {
  netif_t mni_ni;

  SLIST_ENTRY(mbus_netif) mni_global_link;

  // Replace with netif output()?
  pbuf_t *(*mni_output)(struct mbus_netif *mni, pbuf_t *pb);

  uint16_t mni_active_hosts;

  uint32_t mni_rx_packets;
  uint32_t mni_rx_bytes;
  uint32_t mni_rx_crc_errors;
  uint32_t mni_rx_runts;

  uint32_t mni_rx_unknown_opcode;

  uint32_t mni_tx_packets;
  uint32_t mni_tx_bytes;
  uint32_t mni_tx_drops;

#ifdef ENABLE_NET_PCS
  pcs_iface_t *mni_pcs;
#endif

} mbus_netif_t;

#define MBUS_NETIF_ENABLE_PCS 0x1

void mbus_netif_attach(mbus_netif_t *mni, const char *name,
                       uint8_t local_addr, int flags);

pbuf_t *mbus_output(mbus_netif_t *mni, struct pbuf *pb, uint8_t dst_addr);

#define MBUS_OP_PING       0
#define MBUS_OP_PONG       1
#define MBUS_OP_PUB_META   2
#define MBUS_OP_PUB_DATA   3

#define MBUS_OP_DSIG_EMIT  7
// [u8 signal] [u8 valid] [...]

#define MBUS_OP_RPC_RESOLVE         8
// [u8 txid] [name ...]

#define MBUS_OP_RPC_RESOLVE_REPLY   9
// [u8 txid] ([u32 method id] | [])

#define MBUS_OP_RPC_INVOKE          10
// [u8 txid] [u32 method id] [var in-data]

#define MBUS_OP_RPC_ERR             11
// [u8 txid] [s32 errcode]

#define MBUS_OP_RPC_REPLY           12
// [u8 txid] [var out-data]
