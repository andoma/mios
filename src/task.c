#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "task.h"
#include "sys.h"
#include "irq.h"
#include "cpu.h"
#include "mios.h"

static struct task_queue readyqueue = TAILQ_HEAD_INITIALIZER(readyqueue);

#define STACK_GUARD 0xbadc0de

inline task_t *
task_current(void)
{
  return curcpu()->sched.current;
}

void
task_init_cpu(sched_cpu_t *sc, const char *cpu_name)
{
  sc->current = &sc->idle;
  sc->idle.t_state = TASK_STATE_ZOMBIE;
  snprintf(sc->idle.t_name, sizeof(sc->idle.t_name),
           "idle_%s", cpu_name);
}


void *
task_switch(void *cur_sp)
{
  cpu_t *cpu = curcpu();
  task_t *const curtask = task_current();
  curtask->t_sp = cur_sp;

  if(cur_sp < (void *)curtask->t_stack) {
    panic("Stack overflow");
  }

  int s = irq_forbid(IRQ_LEVEL_SCHED);

  if(curtask->t_state == TASK_STATE_RUNNING) {
    // Task should be running, re-insert in readyqueue
    TAILQ_INSERT_TAIL(&readyqueue, curtask, t_link);
  }

  task_t *t = TAILQ_FIRST(&readyqueue);
  if(t == NULL) {
    t = &cpu->sched.idle;
  } else {
    TAILQ_REMOVE(&readyqueue, t, t_link);
  }

#if 0
  printf("Switch from %s [sp:%p] to %s [sp:%p] s=0x%x\n",
         curtask->t_name, curtask->t_sp,
         t->t_name, t->t_sp,
         s);
#endif

  irq_permit(s);

  cpu->sched.current = t;
  cpu_fpu_enable(cpu->sched.current_fpu == t);

  return t->t_sp;
}


static void
task_end(void)
{
  cpu_t *cpu = curcpu();
  task_t *const curtask = task_current();

  int s = irq_forbid(IRQ_LEVEL_SWITCH);

  if(cpu->sched.current_fpu == curtask) {
    cpu->sched.current_fpu = NULL;
    cpu_fpu_enable(0);
  }

  irq_permit(s);

  curtask->t_state = TASK_STATE_ZOMBIE;
  schedule();
  irq_lower();
  while(1) {
  }
}


task_t *
task_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
            const char *name, int flags)
{
  if(stack_size < MIN_STACK_SIZE)
    stack_size = MIN_STACK_SIZE;

  size_t fpu_ctx_size = 0;

  if(flags & TASK_FPU) {
    fpu_ctx_size += FPU_CTX_SIZE;
  }

  task_t *t = malloc(sizeof(task_t) + stack_size + fpu_ctx_size);
  strlcpy(t->t_name, name, sizeof(t->t_name));

  uint32_t *stack_bottom = (void *)t->t_stack;
  memset(stack_bottom, 0xbb, stack_size);
  *stack_bottom = STACK_GUARD;

  t->t_state = 0;

  if(flags & TASK_FPU) {
    t->t_fpuctx = (void *)t->t_stack + stack_size;
    cpu_fpu_ctx_init(t->t_fpuctx);
  } else {
    t->t_fpuctx = NULL;
  }

  t->t_sp = cpu_stack_init((void *)t->t_stack + stack_size, entry, arg,
                           task_end);

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
    TAILQ_REMOVE(waitable, t, t_link);
    t->t_state = TASK_STATE_RUNNING;
    TAILQ_INSERT_TAIL(&readyqueue, t, t_link);
    schedule();
    if(!all)
      break;
  }
  irq_permit(s);
}


typedef struct task_sleep {
  task_t *task;
  struct task_queue *waitable;
} task_sleep_t;


static void
task_sleep_timeout(void *opaque)
{
  const task_sleep_t *ts = opaque;
  task_t *t = ts->task;

  const int s = irq_forbid(IRQ_LEVEL_SCHED);

  if(t->t_state == TASK_STATE_SLEEPING) {

    if(ts->waitable != NULL)
      TAILQ_REMOVE(ts->waitable, t, t_link);

    t->t_state = TASK_STATE_RUNNING;
    TAILQ_INSERT_TAIL(&readyqueue, t, t_link);
    schedule();
  }
  irq_permit(s);
}

void
task_sleep_sched_locked(struct task_queue *waitable, int ticks)
{
  task_t *const curtask = task_current();

  timer_t timer;
  task_sleep_t ts;

  assert(curtask->t_state == TASK_STATE_RUNNING);
  curtask->t_state = TASK_STATE_SLEEPING;

  if(ticks) {
    ts.task = curtask;
    ts.waitable = waitable;
    timer.t_cb = task_sleep_timeout;
    timer.t_opaque = &ts;
    timer.t_countdown = 0;
    timer_arm(&timer, ticks);
  }

  if(waitable != NULL) {
    TAILQ_INSERT_TAIL(waitable, curtask, t_link);
  }

  while(curtask->t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }

  if(ticks) {
    timer_disarm(&timer);
  }

}


void
task_sleep(struct task_queue *waitable, int ticks)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  task_sleep_sched_locked(waitable, ticks);
  irq_permit(s);
}


void
sleephz(int ticks)
{
  task_sleep(NULL, ticks);
}


void
mutex_init(mutex_t *m)
{
  TAILQ_INIT(&m->waiters);
  m->owner = NULL;
}



static void
mutex_lock_sched_locked(mutex_t *m)
{
  task_t *const curtask = task_current();

  if(m->owner != NULL) {
    assert(m->owner != curtask);
    curtask->t_state = TASK_STATE_SLEEPING;
    TAILQ_INSERT_TAIL(&m->waiters, curtask, t_link);
    while(m->owner != NULL) {
      schedule();
      irq_permit(irq_lower());
    }
  }
  m->owner = curtask;
}


void
mutex_lock(mutex_t *m)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  mutex_lock_sched_locked(m);
  irq_permit(s);
}


static void
mutex_unlock_sched_locked(mutex_t *m)
{
  task_t *const curtask = task_current();
  assert(m->owner == curtask);
  m->owner = NULL;

  task_t *t = TAILQ_FIRST(&m->waiters);
  if(t != NULL) {
    TAILQ_REMOVE(&m->waiters, t, t_link);
    t->t_state = TASK_STATE_RUNNING;
    TAILQ_INSERT_TAIL(&readyqueue, t, t_link);
    schedule();
  }
}


void
mutex_unlock(mutex_t *m)
{
  int s = irq_forbid(IRQ_LEVEL_SCHED);
  mutex_unlock_sched_locked(m);
  irq_permit(s);
}



void
cond_init(cond_t *c)
{
  TAILQ_INIT(&c->waiters);
}


void
cond_signal(cond_t *c)
{
  task_wakeup(&c->waiters, 0);
}

void
cond_broadcast(cond_t *c)
{
  task_wakeup(&c->waiters, 1);
}

void
cond_wait(cond_t *c, mutex_t *m)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  mutex_unlock_sched_locked(m);
  task_sleep_sched_locked(&c->waiters, 0);
  mutex_lock_sched_locked(m);
  irq_permit(s);
}

