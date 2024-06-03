#include "cpu.h"

#include <malloc.h>
#include <string.h>

#include <mios/timer.h>

struct cpu cpu0;

static void __attribute__((constructor(150)))
cpu_init(void)
{
  const size_t stack_size = 128;

  // Create idle task
  void *sp_bottom = xalloc(stack_size + sizeof(thread_t),
                           CPU_STACK_ALIGNMENT, 0);
  memset(sp_bottom, 0x55, stack_size + sizeof(thread_t));
  void *sp = sp_bottom + stack_size;

  asm volatile ("cps #0x1f ; mov sp, %0; cps #0x13" : : "r" (sp));

  thread_t *t = sp;
  strlcpy(t->t_name, "idle", sizeof(t->t_name));
  t->t_sp_bottom = sp_bottom;

  t->t_task.t_state = TASK_STATE_ZOMBIE;
  t->t_task.t_prio = 0;
  sched_cpu_init(&curcpu()->sched, t);

}


void *
cpu_stack_init(uint32_t *stack, void *(*entry)(void *arg), void *arg,
               void (*thread_exit)(void *))
{
  *--stack = 0x0000001f;  // CPSR (SYS mode)
  *--stack = (uint32_t) entry;
  *--stack = (uint32_t) thread_exit;
  for(int i = 0; i < 13; i++)
    *--stack = 0;
  stack[8] = (uint32_t) arg; // r0
  return stack;
}




void
reboot(void)
{

}
