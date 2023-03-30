#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct dsig_sub dsig_sub_t;

#define DSIG_EMIT_LOCAL  0x1
#define DSIG_EMIT_MBUS   0x2
#define DSIG_EMIT_ALL  (DSIG_EMIT_LOCAL | DSIG_EMIT_MBUS)


void dsig_emit(uint16_t signal, const void *data, size_t len,
               int flags);

void dsig_dispatch(uint16_t signal, const void *data, size_t len);

dsig_sub_t *dsig_sub(uint16_t signal, uint16_t ttl_ms,
                     void (*cb)(void *opaque, const void *data, size_t len),
                     void *opaque);

dsig_sub_t *dsig_sub_all(void (*cb)(void *opaque, const void *data, size_t len,
                                    uint16_t signal),
                         void *opaque);
