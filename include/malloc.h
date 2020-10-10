#pragma once

#include <stddef.h>

void *xalloc(size_t size, size_t alignment, int type)
  __attribute__((malloc,warn_unused_result));

#define MEM_TYPE_DMA   0x1
#define MEM_TYPE_LOCAL 0x2

void heap_add_mem(long start, long end, int type);
