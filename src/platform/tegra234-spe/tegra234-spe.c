#include <stdio.h>
#include <malloc.h>
#include <stdint.h>

#include "reg.h"

static void  __attribute__((constructor(105)))
spe_init_early(void)
{
  uint32_t ramsize = 256 * 1024;
  void *RAM_end = (void *)0x0c480000 + ramsize;
  heap_add_mem(HEAP_START_EBSS, (long)RAM_end, MEM_TYPE_DMA, 10);
}

static void  __attribute__((constructor(1000)))
spe_init_late(void)
{
  uint32_t hidrev = reg_rd(0x100004); // MISCREG_HIDREV_0
  printf("Sensor Processing Engine on Tegra SoC: t%03x-%c%02d\n",
         (hidrev >> 4) & 0xfff,
         ((hidrev >> 18) & 3) + 'A',
         (hidrev >> 16) & 3);
}
