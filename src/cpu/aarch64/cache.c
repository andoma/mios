#include "cache.h"

static inline uint32_t
dcache_line_size(void)
{
  uint64_t ctr;
  asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
  uint32_t n = (ctr >> 16) & 0xF;
  return 4u << n;
}

static inline uint32_t
icache_line_size(void)
{
  uint64_t ctr;
  asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
  uint32_t n = (ctr >> 0) & 0xF;
  return 4u << n;
}


void
cache_op(void *addr, size_t size, uint32_t flags)
{
  uintptr_t a = (uintptr_t)addr;

  const uintptr_t dline = dcache_line_size();
  const uintptr_t iline = icache_line_size();

  uintptr_t ds = a & ~(dline - 1u);
  uintptr_t de = (a + size + (dline - 1u)) & ~(dline - 1u);

  uintptr_t is = a & ~(iline - 1u);
  uintptr_t ie = (a + size + (iline - 1u)) & ~(iline - 1u);

  asm volatile("dsb ishst" ::: "memory");

  if ((flags & DCACHE_CLEAN_INV) == DCACHE_CLEAN_INV) {
    for (uintptr_t i = ds; i < de; i += dline) {
      asm volatile("dc civac, %0" :: "r"(i) : "memory");
    }
  } else if (flags & DCACHE_CLEAN) {
    for (uintptr_t i = ds; i < de; i += dline) {
      asm volatile("dc cvac, %0" :: "r"(i) : "memory");
    }
  } else if (flags & DCACHE_INVALIDATE) {
    for (uintptr_t i = ds; i < de; i += dline) {
      asm volatile("dc ivac, %0" :: "r"(i) : "memory");
    }
  }

  if (flags & ICACHE_FLUSH) {
    for (uintptr_t i = ds; i < de; i += dline) {
      asm volatile("dc cvau, %0" :: "r"(i) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
    for (uintptr_t i = is; i < ie; i += iline) {
      asm volatile("ic ivau, %0" :: "r"(i) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
    asm volatile("isb");
  }

  asm volatile("dsb ish" ::: "memory");
}
