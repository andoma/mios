#pragma once

#include "net/netif.h"

typedef struct can_netif {
  netif_t cni_ni;

} can_netif_t;

void can_netif_attach(can_netif_t *mni, const char *name,
                      const device_class_t *dc);


void can_dsig_emit(uint32_t signal, const void *data, size_t len);
