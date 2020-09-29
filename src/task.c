//#define READYQUEUE_DEBUG
#define NDEBUG

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "task.h"
#include "sys.h"
#include "irq.h"
#include "cpu.h"
#include "mios.h"


#define TASK_PRIOS 32
#define TASK_PRIO_MASK (TASK_PRIOS - 1)

static struct task_queue readyqueue[TASK_PRIOS];
static uint32_t active_queues;

inline task_t *
task_current(void)
{
  return curcpu()->sched.current;
}

void
task_init_cpu(sched_cpu_t *sc, const char *cpu_name, void *sp_bottom)
{
  sc->current = &sc->idle;
  sc->idle.t_state = TASK_STATE_ZOMBIE;
  sc->idle.t_sp_bottom = sp_bottom;
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
#ifdef READYQUEUE_DEBUG
    if(t == NULL)
      panic("No task on queue %d", which);
#endif
    TAILQ_REMOVE(&readyqueue[which], t, t_link);

    if(TAILQ_FIRST(&readyqueue[which]) == NULL) {
      active_queues &= ~(1 << t->t_prio);
    }
    assert(t->t_state == TASK_STATE_RUNNING);
  }

#if 0
  printf("Switch from %p:%s [sp:%p] to %p:%s [sp:%p bot:%p] s=0x%x\n",
         curtask, curtask->t_name, curtask->t_sp,
         t, t->t_name, t->t_sp, t->t_sp_bottom,
         s);
#endif

  cpu->sched.current = t;
  irq_permit(s);

  cpu_stack_redzone(t);
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

  void *sp_bottom = memalign(stack_size + fpu_ctx_size + sizeof(task_t),
                             CPU_STACK_ALIGNMENT);
  void *sp = sp_bottom + stack_size + fpu_ctx_size;
  task_t *t = sp;
  strlcpy(t->t_name, name, sizeof(t->t_name));

  t->t_state = 0;
  t->t_prio = prio;

  if(flags & TASK_FPU) {
    t->t_fpuctx = sp_bottom + stack_size;
    cpu_fpu_ctx_init(t->t_fpuctx);
  } else {
    t->t_fpuctx = NULL;
  }
  t->t_sp = cpu_stack_init(sp, entry, arg, task_end);
  t->t_sp_bottom = sp_bottom;
#if 0
  printf("Created new task sp_bottom:%p sp:%p t:%p FPU:%p\n",
         sp_bottom, sp, t, t->t_fpuctx);
#endif
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


static int
task_sleep_abs_sched_locked(struct task_queue *waitable, int64_t deadline)
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

  ts.task = curtask;
  ts.waitable = waitable;
  timer.t_cb = task_sleep_timeout;
  timer.t_opaque = &ts;
  timer.t_expire = 0;
  timer_arm_abs(&timer, deadline);

  if(waitable != NULL) {
    TAILQ_INSERT_TAIL(waitable, curtask, t_link);
  }

  while(curtask->t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }

  return timer_disarm(&timer);
}


static void
task_sleep_sched_locked(struct task_queue *waitable)
{
  task_t *const curtask = task_current();

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

  if(waitable != NULL) {
    TAILQ_INSERT_TAIL(waitable, curtask, t_link);
  }

  while(curtask->t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }
}


int
task_sleep(struct task_queue *waitable, int useconds)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  const int64_t deadline = clock_get_irq_blocked() + useconds;
  const int r = task_sleep_abs_sched_locked(waitable, deadline);
  irq_permit(s);
  return r;
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


static void
task_sleep_until(uint64_t deadline)
{
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
}


void
sleep_until(uint64_t deadline)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  task_sleep_until(deadline);
  irq_permit(s);
}


void
usleep(unsigned int useconds)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  task_sleep_until(clock_get_irq_blocked() + useconds);
  irq_permit(s);
}


void
sleep(unsigned int sec)
{
  usleep(sec * 1000000);
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
  assert(m->owner == task_current());
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
  task_sleep_sched_locked(&c->waiters);
  mutex_lock_sched_locked(m);
  irq_permit(s);
}


int
cond_wait_timeout(cond_t *c, mutex_t *m, uint64_t deadline)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  mutex_unlock_sched_locked(m);
  int r = task_sleep_abs_sched_locked(&c->waiters, deadline);
  mutex_lock_sched_locked(m);
  irq_permit(s);
  return r;
}


static void __attribute__((constructor(101)))
task_init(void)
{
  for(int i = 0; i < TASK_PRIOS; i++)
    TAILQ_INIT(&readyqueue[i]);
}
