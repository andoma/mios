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

// Runs before stm32f4_init() (constructor 120). CCM is not cleared on reset
// and its first bytes hold cpu_t (hardwired via the curcpu() macro in
// stm32f4_ccm.h). Clear it here so sched.current reads as NULL and
// thread_current() does not return garbage before cpu_init() (constructor
// 150) sets it up -- this makes evlog() safe for early callers such as
// crashlog_recover() and cmdline_init(). The crashlog lives at the end of
// CCM and is left intact. CCMDATARAMEN is enabled out of reset, but enable
// it explicitly too.
static void  __attribute__((constructor(118)))
stm32f4_ccm_init(void)
{
  clk_enable(CLK_CCMDATARAMEN);
  memset(curcpu(), 0, sizeof(cpu_t));

  heap_add_mem(0x10000000 + sizeof(cpu_t), CRASHLOG_ADDR,
               MEM_TYPE_LOCAL, 5);

  crashlog_recover();
}
