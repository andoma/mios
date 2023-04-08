#include <stdio.h>
#include <malloc.h>

#include <mios/sys.h>

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


reset_reason_t
sys_get_reset_reason(void)
{
  reset_reason_t r = 0;
  const uint32_t flags = reg_rd(0x40000400);

  if(flags & (1 << 0))
    r |= RESET_REASON_EXT_RESET;
  if(flags & (1 << 1))
    r |= RESET_REASON_WATCHDOG;
  if(flags & (1 << 2))
    r |= RESET_REASON_SW_RESET;
  if(flags & (1 << 3))
    r |= RESET_REASON_CPU_LOCKUP;
  if(flags & (1 << 16))
    r |= RESET_REASON_GPIO;
  if(flags & (1 << 17))
    r |= RESET_REASON_COMPARATOR;
  if(flags & (1 << 18))
    r |= RESET_REASON_DEBUG;
  if(flags & (1 << 19))
    r |= RESET_REASON_NFC;
  return r;
}
