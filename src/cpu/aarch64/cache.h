#pragma once

#include <stddef.h>
#include <stdint.h>


#define DCACHE_CLEAN        (1u << 0)
#define DCACHE_INVALIDATE   (1u << 1)
#define DCACHE_CLEAN_INV    (DCACHE_CLEAN | DCACHE_INVALIDATE)
#define ICACHE_FLUSH        (1u << 2)  // Clean D$ to PoU + invalidate I$ to PoU

void cache_op(void *addr, size_t size, uint32_t flags);
