#pragma once

#include "net/netif.h"

typedef struct can_netif {
  netif_t cni_ni;
} can_netif_t;

void can_netif_attach(can_netif_t *mni, const char *name,
                      const device_class_t *dc,
                      const struct dsig_output_filter *output_filter);
