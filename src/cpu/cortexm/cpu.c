#include "cpu.h"

cpu_t cpu0 = {
  .name = "cpu0",
};

static volatile unsigned int * const SHCSR = (unsigned int *)0xe000ed24;


static void __attribute__((constructor(150)))
cpu_init(void)
{
  task_init_cpu(&cpu0.sched, cpu0.name);
  *SHCSR |= 0x7 << 16; // Enable UsageFault, BusFault, MemFault handlers
}


void *
cpu_stack_init(uint32_t *stack, void *(*entry)(void *arg), void *arg,
               void (*thread_exit)(void))
{
  *--stack = 0x21000000;  // PSR
  *--stack = (uint32_t) entry;
  *--stack = (uint32_t) thread_exit;
  for(int i = 0; i < 13; i++)
    *--stack = 0;
  stack[8] = (uint32_t) arg; // r0
  return stack;
}
