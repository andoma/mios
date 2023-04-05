#pragma once

#include <mios/error.h>

#include <stddef.h>
#include <stdint.h>

struct pbuf;

struct pbuf *mbus_rpc_dispatch(struct pbuf *pb, uint8_t src_addr,
                               uint16_t flow);

error_t mbus_rpc_call(uint8_t addr, const char *method,
                      const void *args, size_t argsize,
                      int wait);
