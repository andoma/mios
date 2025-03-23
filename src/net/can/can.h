#pragma once

#include "net/netif.h"

typedef struct can_netif {
  netif_t cni_ni;

  pbuf_t *(*cni_output)(struct can_netif *cni, pbuf_t *pb, uint32_t id);

  error_t (*cni_low_latency_output)(struct can_netif *cni, const void *buf,
                                    size_t len, uint32_t id);

  uint16_t cni_low_latency_output_timestamp;

} can_netif_t;

struct dsig_filter;

void can_netif_attach(can_netif_t *mni, const char *name,
                      const device_class_t *dc,
                      const struct dsig_filter *output_filter);
