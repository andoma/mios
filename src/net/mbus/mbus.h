#pragma once

#include "net/netif.h"

struct mbus_flow;

typedef struct mbus_netif {
  netif_t mni_ni;

  SLIST_ENTRY(mbus_netif) mni_global_link;

  // Replace with netif output()?
  pbuf_t *(*mni_output)(struct mbus_netif *mni, pbuf_t *pb);

  uint32_t mni_active_hosts;

  uint32_t mni_rx_packets;
  uint32_t mni_rx_bytes;
  uint32_t mni_rx_crc_errors;
  uint32_t mni_rx_runts;

  uint32_t mni_rx_unknown_opcode;

  uint32_t mni_tx_packets;
  uint32_t mni_tx_bytes;
  uint32_t mni_tx_qdrops;
  uint32_t mni_tx_fail;

} mbus_netif_t;

void mbus_set_host_address(uint8_t addr);

void mbus_print_info(mbus_netif_t *mni, struct stream *st);

void mbus_netif_attach(mbus_netif_t *mni, const char *name,
                       const device_class_t *dc);

void mbus_netif_detach(mbus_netif_t *mni);

pbuf_t *mbus_output(pbuf_t *pb) __attribute__((warn_unused_result));

pbuf_t *mbus_output_flow(pbuf_t *pb, const struct mbus_flow *mf)
  __attribute__((warn_unused_result));

void mbus_send(pbuf_t *pb);
