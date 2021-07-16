#pragma once

#include "net/netif.h"

typedef struct mbus_netif {
  netif_t mni_ni;

  void (*mni_output)(struct mbus_netif *mni, pbuf_t *pb);

  uint8_t mni_hdr_len;

} mbus_netif_t;

void mbus_netif_attach(mbus_netif_t *mni, const char *name,
                       uint8_t local_addr);


#define MBUS_OP_PING       0
#define MBUS_OP_PONG       1
#define MBUS_OP_TLM_NAME   2
#define MBUS_OP_TLM_FP32   3
#define MBUS_OP_TLM_FP24   4
#define MBUS_OP_TLM_FP16   5
#define MBUS_OP_TLM_UINT16 6

#define MBUS_RPC_CALL      8
#define MBUS_RPC_ERR       9
#define MBUS_RPC_REPLY     10


