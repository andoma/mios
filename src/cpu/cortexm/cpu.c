#include <string.h>
#include <malloc.h>
#include "cpu.h"

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
  void *sp_bottom = xalloc(stack_size + sizeof(task_t),
                                CPU_STACK_ALIGNMENT, 0);
  void *sp = sp_bottom + stack_size;
  asm volatile ("msr psp, %0" : : "r" (sp));

  task_t *t = sp;
  strlcpy(t->t_name, "idle", sizeof(t->t_name));
  t->t_sp_bottom = sp_bottom;

  t->t_state = TASK_STATE_ZOMBIE;
  sched_cpu_init(&curcpu()->sched, t);

}


void *
cpu_stack_init(uint32_t *stack, void *(*entry)(void *arg), void *arg,
               void (*thread_exit)(void))
{
  stack = (uint32_t *)(((intptr_t)stack) & ~7);
  *--stack = 0x21000000;  // PSR
  *--stack = (uint32_t) entry;
  *--stack = (uint32_t) thread_exit;
  for(int i = 0; i < 13; i++)
    *--stack = 0;
  stack[8] = (uint32_t) arg; // r0
  return stack;
}

void
cpu_fpu_ctx_init(int *ctx)
{
  memset(ctx, 0, sizeof(int) * 32);
  ctx[32] = 1 << 24; // Enable flush-to-zero
}


void
halt(const char *msg)
{
  __asm("bkpt 1");
}
