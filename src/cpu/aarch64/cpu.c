#include <stdint.h>
#include <mios/task.h>

#include <malloc.h>
#include <string.h>
#include <stdio.h>

#include "cpu.h"

#include "reg.h"

struct cpu cpu0;


/**
   Stack frame:

   ELR
   SPSR
   x1
   x0
   ...

*/

void *
cpu_stack_init(uint64_t *stack, void *(*entry)(void *arg), void *arg,
               void (*thread_exit)(void *))
{
  *--stack = (intptr_t)entry;
  *--stack = 0x60000344;
  *--stack = (intptr_t)0;   // r1
  *--stack = (intptr_t)arg; // r0
  for(size_t i = 2; i < 31; i++)
    *--stack = 0;
  *--stack = (intptr_t)thread_exit;
  return stack;
}


static void __attribute__((constructor(150)))
cpu_init(void)
{
  const size_t stack_size = 1024;

  // Create idle task
  void *sp_bottom = xalloc(stack_size + sizeof(thread_t),
                           CPU_STACK_ALIGNMENT, 0);
  memset(sp_bottom, 0x55, stack_size + sizeof(thread_t));
  void *sp = sp_bottom + stack_size;
  asm volatile ("msr sp_el0, %0\n\t" : : "r" (sp));
  thread_t *t = sp;
  strlcpy(t->t_name, "idle", sizeof(t->t_name));
  t->t_sp_bottom = sp_bottom;
  t->t_stream = NULL;
  t->t_task.t_state = TASK_STATE_ZOMBIE;
  t->t_task.t_prio = 0;
  sched_cpu_init(&curcpu()->sched, t);
}
