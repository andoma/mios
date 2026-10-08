#include <string.h>
#include <stdarg.h>
#include <malloc.h>

#include <mios/cli.h>

#include "cpu.h"
#include <mios/mios.h>

// If curcpu() is not a macro defined by the platform we define a
// inline function in cortexm.h that expects a global cpu0 to exist
//
#ifndef curcpu
struct cpu cpu0;
#endif

static void __attribute__((constructor(150)))
cpu_init(void)
{
  const size_t stack_size = 128;

  // Create idle task
  void *sp_bottom = xalloc(stack_size + sizeof(thread_t),
                           CPU_STACK_ALIGNMENT, 0);
  memset(sp_bottom, 0x55, stack_size + sizeof(thread_t));
  void *sp = sp_bottom + stack_size;
  asm volatile ("msr psp, %0" : : "r" (sp));

  thread_t *t = sp;
  strlcpy(t->t_name, "idle", sizeof(t->t_name));
  t->t_sp_bottom = sp_bottom;
  t->t_stream = NULL;
#ifdef HAVE_FPU
  t->t_fpuctx = NULL;
#endif
  t->t_task.t_state = TASK_STATE_ZOMBIE;
  t->t_task.t_prio = 0;
  sched_cpu_init(&curcpu()->sched, t);

}


/**
 * Register layout on stack when task is not running
 *
 *   [15] xPSR   ---+  8x32 bytes saved by the CPU on exception entry
 *   [14] PC        |
 *   [13] LR        |
 *   [12] R12       |
 *   [11] R3        |
 *   [10] R2        |
 *    [9] R1        |
 *    [8] R0     ---+
 *    [7] R11    ---+  8x32 bytes stored by Mios exception handlers
 *    [6] R10       |  - Pushed/poped by pendsv (task-switching)
 *    [5] R9        |  - Regular ISRs do not store these as they are
 *    [4] R8        |    callee saved (the C ISR code will push if needed)
 *    [3] R7        |
 *    [2] R6        |
 *    [1] R5        |
 *    [0] R4     ---+ <- Thus, for a non-running task SP points here
 */

void *
cpu_stack_init(uint32_t *stack, void *entry,
               void (*thread_exit)(void *), int nargs, va_list ap)
{
  stack = (uint32_t *)(((intptr_t)stack) & ~7);
  *--stack = 0x21000000;  // PSR
  *--stack = (uint32_t) entry;
  *--stack = (uint32_t) thread_exit;
  for(int i = 0; i < 13; i++)
    *--stack = 0;
  for(int i = 0; i < nargs; i++)
    stack[8 + i] = (uint32_t)va_arg(ap, uintptr_t); // r0-r3
  return stack;
}

void
cpu_fpu_ctx_init(int *ctx)
{
  memset(ctx, 0, sizeof(int) * 32);
  ctx[32] = 1 << 24; // Enable flush-to-zero
}

#ifdef HAVE_FPU
void
cpu_fpu_switch(thread_t *t)
{
  if(t->t_fpuctx == NULL) {
    // Keep the previous owner's state in the registers while integer-only
    // threads run, but continue to trap accidental FP use by those threads.
    cpu_fpu_enable(0);
    return;
  }
  cpu_t *cpu = curcpu();
  cpu_fpu_enable(1);
  if(cpu->sched.current_fpu == t)
    return;

  if(cpu->sched.current_fpu) {
    int32_t *ctx = cpu->sched.current_fpu->t_fpuctx;
    asm volatile("vstm %0, {s0-s15}" :: "r"(ctx) : "memory");
    asm volatile("vstm %0, {s16-s31}" :: "r"(ctx + 16) : "memory");
    uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    ctx[32] = fpscr;
  }
  const int32_t *ctx = t->t_fpuctx;
  asm volatile("vldm %0, {s0-s15}" :: "r"(ctx) : "memory");
  asm volatile("vldm %0, {s16-s31}" :: "r"(ctx + 16) : "memory");
  asm volatile("vmsr fpscr, %0" :: "r"(ctx[32]));
  cpu->sched.current_fpu = t;
}

#ifdef CPU_FPU_ICI_RESUME
uint32_t cpu_fpu_ici_restores;

void
cpu_fpu_resume(thread_t *t)
{
  cpu_t *cpu = curcpu();
  if(cpu->sched.current_fpu == t) {
    cpu_fpu_enable(1);
    return;
  }

  if(t->t_fpuctx != NULL) {
    // PendSV saves r4-r11 before the basic hardware frame; xPSR is word 15.
    // IT and ICI share bits. ICI is nonzero only when IT[3:0] is zero.
    const uint32_t xpsr = ((const uint32_t *)t->t_sp)[15];
    if((xpsr & 0x0000f000) && !(xpsr & 0x06000c00)) {
      // An FP multiple transfer may already be partway through execution.
      // Returning to it with CP10/11 disabled can raise INVSTATE, not the
      // NOCP used for lazy acquisition. Restore before exception return;
      // preserve the PC, SP and ICI exactly. Conservatively cover integer
      // continuations too, avoiding an instruction fetch in the scheduler.
      cpu_fpu_switch(t);
      cpu_fpu_ici_restores++;
      return;
    }
  }
  // Normal instruction boundaries still acquire the FPU lazily via NOCP.
  cpu_fpu_enable(0);
}
#endif
#endif


void
halt(const char *msg)
{
  // With a debugger attached, stop where the problem is. Without one a
  // breakpoint instruction faults, and the part then sits dead until
  // the watchdog gets it, thirty seconds later, with "watchdog" as the
  // only record. Reboot instead: whatever the panic path wrote to the
  // crash log is read back on the way up.
  static volatile uint32_t *const DHCSR = (volatile uint32_t *)0xe000edf0;
  if(*DHCSR & 1) // C_DEBUGEN
    __asm("bkpt 1");
  reboot();
}

void
reboot(void)
{
  static volatile uint32_t *const AIRCR  = (volatile uint32_t *)0xe000ed0c;
  *AIRCR = 0x05fa0004;
  while(1) {}
}

void   __attribute__((weak, noreturn))
cpu_idle(void)
{
  while(1) {
    asm volatile ("wfi");
  }
}


void __attribute__((weak))
dcache_op(void *addr, size_t size, uint32_t flags)
{

}


void __attribute__((weak))
icache_invalidate(void)
{

}
