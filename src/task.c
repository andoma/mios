#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

#include "task.h"
#include "sys.h"
#include "irq.h"

static struct task_queue readyqueue = TAILQ_HEAD_INITIALIZER(readyqueue);

static task_t idle_task = {
  .t_name = "idle",
  .t_state = TASK_STATE_ZOMBIE,
};

#define STACK_GUARD 0xbadc0de

struct task *curtask = &idle_task;

void *
sys_switch(void *cur_psp)
{
  curtask->t_psp = cur_psp;

  int s = irq_forbid(IRQ_LEVEL_SCHED);

  if(curtask->t_state == TASK_STATE_RUNNING) {
    // Task should be running, re-insert in readyqueue
    TAILQ_INSERT_TAIL(&readyqueue, curtask, t_link);
  }

  task_t *t = TAILQ_FIRST(&readyqueue);
  if(t == NULL) {
    t = &idle_task;
  } else {
    TAILQ_REMOVE(&readyqueue, t, t_link);
  }

#if 1
  printf("Switch from %s to %s\n", curtask->t_name, t->t_name);
#endif

  irq_permit(s);

  curtask = t;
  return t->t_psp;
}


static void
task_end(void)
{
  curtask->t_state = TASK_STATE_ZOMBIE;
  schedule();
  irq_lower();
  while(1) {
  }
}


task_t *
task_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
            const char *name)
{
  assert(stack_size >= 256);
  task_t *t = malloc(sizeof(task_t) + stack_size);
  t->t_name = name;
  t->t_waitable = NULL;

  uint32_t *stack_bottom = (void *)t->t_stack;
  *stack_bottom = STACK_GUARD;

  uint32_t *stack = (void *)t->t_stack + stack_size;

  *--stack = 0x21000000;  // PSR
  *--stack = (uint32_t) entry;
  *--stack = (uint32_t) task_end;
  for(int i = 0; i < 13; i++)
    *--stack = 0;
  t->t_psp = stack;
  stack[8] = (uint32_t) arg; // r0
  printf("Creating task %p %s\n", t, name);

  t->t_state = 0;

  int s = irq_forbid(IRQ_LEVEL_SCHED);
  TAILQ_INSERT_TAIL(&readyqueue, t, t_link);
  irq_permit(s);

  schedule();
  return t;
}


void
task_wakeup(struct task_queue *waitable, int all)
{
  int s = irq_forbid(IRQ_LEVEL_SCHED);

  task_t *t;
  while((t = TAILQ_FIRST(waitable)) != NULL) {
    assert(t->t_state == TASK_STATE_SLEEPING);
    assert(t->t_waitable == waitable);
    TAILQ_REMOVE(waitable, t, t_link);
    t->t_waitable = NULL;
    t->t_state = TASK_STATE_RUNNING;
    TAILQ_INSERT_TAIL(&readyqueue, t, t_link);
    schedule();
    if(!all)
      break;
  }
  irq_permit(s);
}


static void
task_sleep_timer(void *opaque)
{
  task_t *t = opaque;

  int s = irq_forbid(IRQ_LEVEL_SCHED);

  if(t->t_state == TASK_STATE_SLEEPING) {

    if(t->t_waitable != NULL)
      TAILQ_REMOVE(t->t_waitable, t, t_link);

    t->t_state = TASK_STATE_RUNNING;
    TAILQ_INSERT_TAIL(&readyqueue, t, t_link);
    schedule();
  }
  irq_permit(s);
}



void
task_sleep(struct task_queue *waitable, int ticks)
{
  timer_t timer;
  timer.t_cb = task_sleep_timer;
  timer.t_opaque = curtask;
  timer.t_countdown = 0;

  int s = irq_forbid(IRQ_LEVEL_SCHED);
  assert(curtask->t_state == TASK_STATE_RUNNING);
  curtask->t_state = TASK_STATE_SLEEPING;

  if(ticks)
    timer_arm(&timer, ticks);

  if(waitable != NULL) {
    curtask->t_waitable = waitable;
    TAILQ_INSERT_TAIL(waitable, curtask, t_link);
  }

  while(curtask->t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }

  timer_disarm(&timer);

  irq_permit(s);
}


void
sleephz(int ticks)
{
  task_sleep(NULL, ticks);
}
