#include <malloc.h>

static void  __attribute__((constructor(120)))
aarch64_init(void)
{
  heap_add_mem(0x41000000, 0x42000000, MEM_TYPE_DMA);

  extern void heap_dump_all(void);

  heap_dump_all();

}
