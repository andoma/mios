#pragma once

#include <stddef.h>

void *xalloc(size_t size, size_t alignment, unsigned int type_flags)
  __attribute__((malloc,warn_unused_result));

// Return 1 if failed to free memory (locked and will not block)
int free_try(void *ptr);

#define MEM_TYPE_DMA   0x1
#define MEM_TYPE_LOCAL 0x2

#define MEM_MAY_FAIL   0x80000000

#define HEAP_START_EBSS __SIZE_MAX__

void heap_add_mem(long start, long end, int type);
