#pragma once

#include <stdint.h>

typedef struct vllp vllp_t;

typedef struct vllp_channel vllp_channel_t;

vllp_t *vllp_server_create(uint32_t txid, uint32_t rxid, uint8_t mtu);
