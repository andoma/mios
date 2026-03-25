#include <stdint.h>
#include <stdarg.h>
#include <mios/task.h>

#include <malloc.h>
#include <string.h>
#include <stdio.h>

#include "cpu.h"

#include "reg.h"

struct cpu cpu0;


/**
   Stack frame (34 x 64-bit words):

   [33] ELR
   [32] SPSR
   [31] x1
   [30] x0
   [29] x2    ...  [28] x3
   ...
   [1]  (padding)
   [0]  x30 (LR)
*/

void *
cpu_stack_init(uint64_t *stack, void *entry,
               void (*thread_exit)(void *), int nargs, va_list ap)
{
  stack -= 34;
  memset(stack, 0, 8 * 34);
  stack[33] = (intptr_t)entry;
  stack[32] = 0x60000344;
  stack[0]  = (intptr_t)thread_exit;

  // x0 at [30], x1 at [31], x2 at [29], x3 at [28], ...
  static const int arg_idx[] = { 30, 31, 29, 28 };
  for(int i = 0; i < nargs; i++)
    stack[arg_idx[i]] = va_arg(ap, uintptr_t);
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


__attribute__((noreturn, weak))
void
cpu_idle(void)
{
  while(1) {
    asm volatile("wfi");
  }
}
