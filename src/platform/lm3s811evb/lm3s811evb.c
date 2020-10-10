#include <stdio.h>
#include <malloc.h>


static void  __attribute__((constructor(120)))
lm3s811evb_init(void)
{
  extern unsigned long _ebss;

  printf("\nPlatform: lm3s811evb\n");
  heap_add_mem((long)&_ebss, 0x20002000, 0);
}
