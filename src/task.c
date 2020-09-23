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

// #define READYQUEUE_DEBUG

#define TASK_PRIOS 32
#define TASK_PRIO_MASK (TASK_PRIOS - 1)

static struct task_queue readyqueue[TASK_PRIOS];
static uint32_t active_queues;

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


static void
readyqueue_insert(task_t *t, const char *whom)
{
#ifdef READYQUEUE_DEBUG
  for(int i = 0; i < TASK_PRIOS; i++) {
    task_t *x;
    TAILQ_FOREACH(x, &readyqueue[i], t_link) {
      if(x == t) {
        panic("%s: Inserting task %p on readyqueue but it's already there",
              whom, t);
      }
    }
  }
#endif
  TAILQ_INSERT_TAIL(&readyqueue[t->t_prio], t, t_link);
  active_queues |= 1 << t->t_prio;
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
    readyqueue_insert(curtask, "task_switch");
  }

  task_t *t;
  int which = __builtin_clz(active_queues);
  if(which == 32) {
    t = &cpu->sched.idle;
  } else {
    which = 31 - which;
    t = TAILQ_FIRST(&readyqueue[which]);
    if(t == NULL)
      panic("No task on queue %d", which);
    TAILQ_REMOVE(&readyqueue[which], t, t_link);

    if(TAILQ_FIRST(&readyqueue[which]) == NULL) {
      active_queues &= ~(1 << t->t_prio);
    }
    assert(t->t_state == TASK_STATE_RUNNING);
  }

#if 0
  printf("Switch from %p:%s [sp:%p] to %p:%s [sp:%p] s=0x%x\n",
         curtask, curtask->t_name, curtask->t_sp,
         t, t->t_name, t->t_sp,
         s);
#endif

  cpu->sched.current = t;
  irq_permit(s);

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
            const char *name, int flags, unsigned int prio)
{
  prio &= TASK_PRIO_MASK;

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
  t->t_prio = prio;

  if(flags & TASK_FPU) {
    t->t_fpuctx = (void *)t->t_stack + stack_size;
    cpu_fpu_ctx_init(t->t_fpuctx);
  } else {
    t->t_fpuctx = NULL;
  }
  t->t_sp = cpu_stack_init((void *)t->t_stack + stack_size, entry, arg,
                           task_end);

  int s = irq_forbid(IRQ_LEVEL_SCHED);
  TAILQ_INSERT_TAIL(&readyqueue[t->t_prio], t, t_link);
  active_queues |= 1 << t->t_prio;
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
    if(t != task_current())
      readyqueue_insert(t, "wakeup");
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
    if(t != task_current())
      readyqueue_insert(t, "sleep-timo");
    schedule();
  }
  irq_permit(s);
}

int
task_sleep_sched_locked(struct task_queue *waitable, int us)
{
  task_t *const curtask = task_current();

  timer_t timer;
  task_sleep_t ts;

  assert(curtask->t_state == TASK_STATE_RUNNING);
  curtask->t_state = TASK_STATE_SLEEPING;

#ifdef READYQUEUE_DEBUG
  for(int i = 0; i < TASK_PRIOS; i++) {
    task_t *x;
    TAILQ_FOREACH(x, &readyqueue[i], t_link) {
      assert(x != curtask);
    }
  }
#endif

  if(us) {
    ts.task = curtask;
    ts.waitable = waitable;
    timer.t_cb = task_sleep_timeout;
    timer.t_opaque = &ts;
    timer.t_expire = 0;
    timer_arm(&timer, us);
  }

  if(waitable != NULL) {
    TAILQ_INSERT_TAIL(waitable, curtask, t_link);
  }

  while(curtask->t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }

  if(us) {
    return timer_disarm(&timer);
  }
  return 0;
}


int
task_sleep(struct task_queue *waitable, int ticks)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  const int r = task_sleep_sched_locked(waitable, ticks);
  irq_permit(s);
  return r;
}


void
usleep(unsigned int us)
{
  task_sleep(NULL, us);
}

void
sleep(unsigned int sec)
{
  task_sleep(NULL, sec * 1000000);
}


static void
task_sleep_timeout2(void *opaque)
{
  task_t *t = opaque;
  const int s = irq_forbid(IRQ_LEVEL_SCHED);

  assert(t->t_state == TASK_STATE_SLEEPING);
  t->t_state = TASK_STATE_RUNNING;
  if(t != task_current())
    readyqueue_insert(t, "sleep-timo2");
  schedule();
  irq_permit(s);
}


void
sleep_until(uint64_t deadline)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);

  task_t *const curtask = task_current();

  timer_t timer;

  assert(curtask->t_state == TASK_STATE_RUNNING);
  curtask->t_state = TASK_STATE_SLEEPING;

#ifdef READYQUEUE_DEBUG
  for(int i = 0; i < TASK_PRIOS; i++) {
    task_t *x;
    TAILQ_FOREACH(x, &readyqueue[i], t_link) {
      assert(x != curtask);
    }
  }
#endif

  timer.t_cb = task_sleep_timeout2;
  timer.t_opaque = curtask;
  timer.t_expire = 0;
  timer_arm_abs(&timer, deadline);

  while(curtask->t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }

  irq_permit(s);
  return;
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
    if(t != task_current())
      readyqueue_insert(t, "mutex_unlock");
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


static void __attribute__((constructor(101)))
task_init(void)
{
  for(int i = 0; i < TASK_PRIOS; i++)
    TAILQ_INIT(&readyqueue[i]);
}
