#pragma once

#include <stddef.h>
#include <stdint.h>

void icache_invalidate(void);

#define DCACHE_CLEAN      0x1
#define DCACHE_INVALIDATE 0x2

void dcache_op(void *addr, size_t size, uint32_t flags);

void icache_disable(void);

void icache_enable(void);

void dcache_disable(void);

void dcache_enable(void);
