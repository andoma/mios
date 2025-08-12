#include "cache.h"

void
icache_invalidate(void)
{
  asm volatile ("ic iallu\n\t"
                "dsb sy\n\t"
                "isb");
}


void
dcache_op(void *addr, size_t size, uint32_t flags)
{
  intptr_t s = (intptr_t)addr & ~63ULL;
  intptr_t e = (intptr_t)(addr + size + 63) & ~63ULL;

  asm volatile("dsb ishst");

  if(flags & DCACHE_CLEAN) {

    for(intptr_t i = s; i < e; i += 64) {
      asm volatile("dc cvau, %0" :: "r"(i));
    }
  }

  asm volatile("dsb ish");
}
