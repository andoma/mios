#pragma once

#include <stdint.h>
#include <mios/error.h>

struct pushpull;
struct block_iface;
struct pbuf;

error_t ota_open_with_args(struct pushpull *s,
                           struct block_iface *partition,
                           int skip_kb,
                           void (*platform_upgrade)(uint32_t flow_header,
                                                    struct pbuf *pb));

