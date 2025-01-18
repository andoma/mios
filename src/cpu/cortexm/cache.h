#pragma once

void icache_invalidate(void);

#define DCACHE_CLEAN      0x1
#define DCACHE_INVALIDATE 0x2

void dcache_op(void *addr, size_t size, uint32_t flags);
