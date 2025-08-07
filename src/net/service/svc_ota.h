#pragma once

#include <stdint.h>
#include <mios/error.h>

struct pushpull;
struct block_iface;
struct pbuf;

error_t ota_open_with_args(struct pushpull *s,
                           struct block_iface *partition,
                           int xfer_skip_kb,
                           int writeout_skip_kb,
                           int blocksize,
                           void (*platform_upgrade)(uint32_t flow_header,
                                                    struct pbuf *pb));

