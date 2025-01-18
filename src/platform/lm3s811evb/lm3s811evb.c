#include <stdio.h>
#include <malloc.h>


static void  __attribute__((constructor(120)))
lm3s811evb_init(void)
{
  printf("\nPlatform: lm3s811evb\n");
  heap_add_mem(HEAP_START_EBSS, 0x20002000, 0, 10);
}
