#include <string.h>

#include "cpu.h"

cpu_t cpu0 = {
  .name = "cpu0",
};



static void __attribute__((constructor(150)))
cpu_init(void)
{
  extern void *idle_stack;
  task_init_cpu(&cpu0.sched, cpu0.name, &idle_stack);
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
