#pragma once

#include <mios/error.h>

struct socket;
struct block_iface;

error_t ota_open_with_args(struct socket *s,
                           struct block_iface *partition,
                           int skip_kb,
                           error_t (*platform_upgrade)(uint32_t flow_header,
                                                       pbuf_t *pb));

