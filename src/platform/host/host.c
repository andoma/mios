#include <stdio.h>
#include <malloc.h>

#include <mios/mios.h>

#include "cpu.h"

static void  __attribute__((constructor(120)))
host_platform_init(void)
{
  printf("\nPlatform: host (Linux x86-64)\n");
  host_map_heap(HOST_HEAP_SIZE);
}
