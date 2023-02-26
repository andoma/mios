#include <stdio.h>
#include <malloc.h>

#include "cpu.h"

#include "nrf52_reg.h"

static void  __attribute__((constructor(102)))
nrf52_init_heap(void)
{
  const uint32_t ramsize = reg_rd(0x1000010c);
  void *RAM_end = (void *)0x20000000 + ramsize * 1024;
  // SRAM1
  heap_add_mem(HEAP_START_EBSS, (long)RAM_end, MEM_TYPE_DMA);
}


static void  __attribute__((constructor(120)))
nrf52_init(void)
{
  const uint32_t part = reg_rd(0x10000100);
  const uint32_t ramsize = reg_rd(0x1000010c);
  const uint32_t flashsize = reg_rd(0x10000110);

  printf("\nnRF%x (%d kB Flash, %d kB RAM)\n", part, flashsize, ramsize);

}
