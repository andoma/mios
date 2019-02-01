#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>


#include "task.h"
#include "sys.h"
#include "irq.h"

static volatile unsigned int * const ICSR = (unsigned int *)0xe000ed04;
static struct task_queue readyqueue = TAILQ_HEAD_INITIALIZER(readyqueue);

static task_t idle_task = {
  .t_name = "idle"
};

#define STACK_GUARD 0xbadc0de

struct task *curtask = &idle_task;

static uint32_t idle_task_stack[16] __attribute__((aligned(8)));

void *
sys_switch(void *cur_psp)
{
  curtask->t_psp = cur_psp;
  curtask->t_basepri = irq_getpri();
  task_t *t = TAILQ_FIRST(&readyqueue);
  if(t != NULL) {
    TAILQ_REMOVE(&readyqueue, t, t_link);
  } else {
    t = &idle_task;
  }
#if 0
  printf("Switch from %s [pri:%x] to %s [pri:%x]\n",
         curtask->t_name, curtask->t_basepri,
         t->t_name, t->t_basepri);
#endif
  curtask = t;
  irq_setpri(t->t_basepri);
  return t->t_psp;
}

void
sys_relinquish(void)
{
  *ICSR = 1 << 28;
}

void
sys_yield(void)
{
  TAILQ_INSERT_TAIL(&readyqueue, curtask, t_link);
  *ICSR = 1 << 28;
}


void
sys_task_start(task_t *t)
{
  TAILQ_INSERT_TAIL(&readyqueue, t, t_link);
}

void
task_end(void)
{
  syscall0(SYS_relinquish);
}

static void
task_wakeup(void *opaque)
{
  task_t *t = opaque;
  TAILQ_INSERT_TAIL(&readyqueue, t, t_link);
  *ICSR = 1 << 28;
}

void
sys_sleep(int ticks)
{
  timer_arm(&curtask->t_timer, ticks);
  sys_relinquish();
}



task_t *
task_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
            const char *name)
{
  assert(stack_size >= 256);
  task_t *t = malloc(sizeof(task_t) + stack_size);
  t->t_basepri = 0;
  t->t_name = name;
  t->t_timer.t_cb = task_wakeup;
  t->t_timer.t_opaque = t;

  uint32_t *stack_bottom = (void *)t->t_stack;
  *stack_bottom = 0xbadc0de;

  uint32_t *stack = (void *)t->t_stack + stack_size;

  *--stack = 0x21000000;
  *--stack = (uint32_t) entry;
  *--stack = (uint32_t) task_end;
  for(int i = 0; i < 13; i++)
    *--stack = 0;
  t->t_psp = stack;
  stack[8] = (uint32_t) arg; // r0
  syscall1(SYS_task_start, (int)t);
  return t;
}


void __attribute__((naked))
multitask(void)
{
  sys_set_control(2);
  sys_isb();
  sys_set_psp(&idle_task_stack[16]);
  sys_isb();
  syscall0(SYS_relinquish);
  while(1) {
    asm volatile ("wfi\n\t");
  }
}

