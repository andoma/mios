#pragma once

#include <stdint.h>

// Hardware Synchronization Primitives

#define NV_ADDRESS_MAP_BPMP_HSP_BASE 0x0d150000
#define NV_ADDRESS_MAP_AON_HSP_BASE  0x0c150000
#define NV_ADDRESS_MAP_TOP0_HSP_BASE 0x03c00000

struct stream;

struct stream *
hsp_mbox_stream(uint32_t rx_hsp_base, uint32_t rx_mbox,
                uint32_t tx_hsp_base, uint32_t tx_mbox);

