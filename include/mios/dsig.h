#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct dsig_sub dsig_sub_t;

#define DSIG_EMIT_LOCAL  0x1
#define DSIG_EMIT_MBUS   0x2
#define DSIG_EMIT_ALL  (DSIG_EMIT_LOCAL | DSIG_EMIT_MBUS)

// TTL is in units of 10ms

void dsig_emit(uint8_t signal, const void *data, size_t len,
               uint8_t ttl, int flags);

void dsig_dispatch(uint8_t signal, const void *data, size_t len,
                   uint8_t ttl, uint8_t src_addr);

dsig_sub_t *dsig_sub(uint8_t signal,
                     void (*cb)(void *opaque, const void *data, size_t len),
                     void *opaque);
