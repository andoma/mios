#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct dsig_sub dsig_sub_t;

void dsig_emit(uint32_t signal, const void *data, size_t len);

dsig_sub_t *dsig_sub(uint32_t signal, uint16_t ttl_ms,
                     void (*cb)(void *opaque, const void *data, size_t len),
                     void *opaque);

dsig_sub_t *dsig_sub_all(void (*cb)(void *opaque, const void *data,
                                    size_t len, uint32_t signal),
                         void *opaque);

struct dsig_filter {
  uint32_t prefix;
  uint8_t prefixlen;
  uint16_t flags;
};

#define DSIG_FLAG_EXTENDED 0x1

#define DSIG_FILTER_END { .prefixlen = 0xff }
