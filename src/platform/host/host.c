#include <stdio.h>
#include <malloc.h>

#include <mios/mios.h>

#include "linux.h"

static void  __attribute__((constructor(120)))
host_platform_init(void)
{
  printf("\nPlatform: host (Linux x86-64)\n");

  void *heap = linux_mmap(NULL, HOST_HEAP_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if(heap == NULL)
    panic("Unable to map heap");

  heap_add_mem((long)heap, (long)heap + HOST_HEAP_SIZE,
               MEM_TYPE_LOCAL | MEM_TYPE_DMA, 10);
}
