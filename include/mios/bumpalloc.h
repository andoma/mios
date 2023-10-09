#pragma once

#include <stddef.h>
#include <stdint.h>

#include "error.h"

typedef struct balloc {
  size_t size;
  size_t used;
  uint8_t data[0];
} balloc_t;

balloc_t *balloc_create(size_t size);

error_t balloc_append_data(balloc_t *ba, const char *src, size_t srclen,
                           void **ptr, size_t *sizep);

void *balloc_alloc(balloc_t *ba, size_t size);
