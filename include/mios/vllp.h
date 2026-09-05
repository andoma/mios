#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct vllp vllp_t;

typedef struct vllp_channel vllp_channel_t;

// Largest message the server can reassemble with the pbuf size this
// build is configured for. A bigger message is dropped (and logged), and
// because the sender gets no acknowledgement it will keep retrying, so
// anything that must get through has to fit. Scales with PBUF_DATA_SIZE,
// which is why it is a query rather than a constant.
size_t vllp_max_message_size(void);

vllp_t *vllp_server_create(uint32_t txid, uint32_t rxid, uint8_t mtu,
                           uint8_t timeout_seconds);
