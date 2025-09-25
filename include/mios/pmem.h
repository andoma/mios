#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
  unsigned long paddr;
  unsigned long size;
  uint32_t type;
} pmem_segment_t;


typedef struct {
  pmem_segment_t *segments;
  size_t count;
  size_t capacity;
  size_t minimum_alignment;
} pmem_t;

void pmem_add(pmem_t *p, unsigned long paddr, unsigned long size,
              uint32_t type);

int pmem_set(pmem_t *p, unsigned long paddr, unsigned long size,
             uint32_t type, int allow_split);

unsigned long pmem_alloc(pmem_t *p, unsigned long size, uint32_t from_type,
                         uint32_t as_type, unsigned long alignment);
