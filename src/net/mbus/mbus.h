#pragma once

#include "net/netif.h"

typedef struct mbus_netif {
  netif_t mni_ni;

  void (*mni_output)(struct mbus_netif *mni, pbuf_t *pb);

  uint8_t mni_hdr_len;

  uint32_t mni_rx_packets;
  uint32_t mni_rx_bytes;
  uint32_t mni_rx_crc_errors;
  uint32_t mni_rx_runts;

  uint32_t mni_rx_unknown_opcode;

  uint32_t mni_tx_packets;
  uint32_t mni_tx_bytes;

} mbus_netif_t;

void mbus_netif_attach(mbus_netif_t *mni, const char *name,
                       uint8_t local_addr);


void mbus_output(mbus_netif_t *mni, struct pbuf *pb, uint8_t dst_addr);

#define MBUS_OP_PING       0
#define MBUS_OP_PONG       1
#define MBUS_OP_TLM_META   2
#define MBUS_OP_TLM_FP32   3
#define MBUS_OP_TLM_FP24   4
#define MBUS_OP_TLM_FP16   5
#define MBUS_OP_TLM_I16    6

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