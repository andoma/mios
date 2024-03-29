#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct dsig_sub dsig_sub_t;

void dsig_emit(uint16_t signal, const void *data, size_t len);

void dsig_dispatch(uint16_t signal, const void *data, size_t len);

dsig_sub_t *dsig_sub(uint16_t signal, uint16_t ttl_ms,
                     void (*cb)(void *opaque, const void *data, size_t len),
                     void *opaque);

dsig_sub_t *dsig_sub_all(void (*cb)(void *opaque, const void *data, size_t len,
                                    uint16_t signal),
                         void *opaque);
