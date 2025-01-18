#pragma once

#include <stddef.h>
#include <stdint.h>

void *xalloc(size_t size, size_t alignment, unsigned int type_flags)
  __attribute__((malloc,warn_unused_result));

// Return 1 if failed to free memory (locked and will not block)
int free_try(void *ptr);

#define MEM_TYPE_LOCAL          0x1
#define MEM_TYPE_DMA            0x2
#define MEM_TYPE_NO_CACHE       0x4
#define MEM_TYPE_VECTOR_TABLE   0x8

#define MEM_MAY_FAIL   0x80000000

#define HEAP_START_EBSS 0xffffffff

// Lower numerical prio means it be tried earlier if no specific
// memory type is requested when allocating
void heap_add_mem(long start, long end, uint8_t type, uint8_t prio);
