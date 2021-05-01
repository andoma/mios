#include <stdio.h>
#include <malloc.h>

#include "stm32f4_clk.h"
#include "cpu.h"

static void  __attribute__((constructor(121)))
stm32f4_ccm_init(void)
{
  // CCM
  // Note: First bytes of CCM are reserved for cpu_t and hardwired to
  // this address via the curcpu() macro in stm32f4_ccm.h

  clk_enable(CLK_CCMDATARAMEN);
  heap_add_mem(0x10000000 + sizeof(cpu_t), 0x10010000, MEM_TYPE_LOCAL);
}
