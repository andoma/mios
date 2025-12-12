#include <stdio.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <sys/param.h>

#include <mios/eventlog.h>

#include "stm32f4_clk.h"
#include "cpu.h"

#define CRASHLOG_SIZE  512
#define CRASHLOG_ADDR  (0x10010000 - CRASHLOG_SIZE)

static void
get_crashlog_stream_prep(void)
{
}

#include "lib/sys/crashlog.c"

static void  __attribute__((constructor(200)))
stm32f4_ccm_init(void)
{
  // CCM
  // Note: First bytes of CCM are reserved for cpu_t and hardwired to
  // this address via the curcpu() macro in stm32f4_ccm.h
  // Last 256 bytes are used as panic-buffer as CCM is not cleared on reset

  clk_enable(CLK_CCMDATARAMEN);
  heap_add_mem(0x10000000 + sizeof(cpu_t), CRASHLOG_ADDR,
               MEM_TYPE_LOCAL, 5);

  crashlog_recover();
}
