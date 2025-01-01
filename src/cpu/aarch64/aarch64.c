#include <malloc.h>

static void  __attribute__((constructor(120)))
aarch64_init(void)
{
  extern void heap_dump_all(void);
  heap_dump_all();

  extern long vectors[];

  asm volatile ("msr vbar_el1, %0\n\t" : : "r" (vectors));

}
