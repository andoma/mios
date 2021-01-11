// #define TASK_DEBUG


#ifndef TASK_DEBUG
#define NDEBUG
#endif

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include <mios/task.h>
#include <mios/mios.h>
#include <mios/cli.h>

#include "sys.h"
#include "irq.h"
#include "cpu.h"

#define TASK_PRIOS 32
#define TASK_PRIO_MASK (TASK_PRIOS - 1)

SLIST_HEAD(task_slist, task);
TAILQ_HEAD(task_queue, task);

int task_trace;


static struct task_queue readyqueue[TASK_PRIOS];
static uint32_t active_queues;
static struct task_slist alltasks;

static mutex_t alltasks_mutex = MUTEX_INITIALIZER("alltasks");
static cond_t task_mgmt_cond = COND_INITIALIZER("taskmgmt");

static task_waitable_t join_wait;

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

#ifdef TASK_DEBUG

static int
task_is_on_queue(task_t *t, struct task_queue *q)
{
  task_t *x;
  TAILQ_FOREACH(x, q, t_ready_link) {
    if(x == t) {
      return 1;
    }
  }
  return 0;
}


static int
task_is_on_list(task_t *t, struct task_list *l)
{
  task_t *x;
  LIST_FOREACH(x, l, t_wait_link) {
    if(x == t) {
      return 1;
    }
  }
  return 0;
}


static int
task_is_on_readyqueue(task_t *t)
{
  for(int i = 0; i < TASK_PRIOS; i++) {
    if(task_is_on_queue(t, &readyqueue[i]))
      return 1;
  }
  return 0;
}
#endif

static void
readyqueue_insert(task_t *t, const char *whom)
{
#ifdef TASK_DEBUG
  if(task_is_on_readyqueue(t)) {
    panic("%s: Inserting task %p on readyqueue but it's already there",
          whom, t);
  }
#endif
  TAILQ_INSERT_TAIL(&readyqueue[t->t_prio], t, t_ready_link);
  active_queues |= 1 << t->t_prio;
}


void *
task_switch(void *cur_sp)
{
  cpu_t *cpu = curcpu();
  task_t *const curtask = task_current();
  curtask->t_sp = cur_sp;

#ifdef TASK_ACCOUNTING
  curtask->t_cycle_acc += cpu_cycle_counter() - curtask->t_cycle_enter;
#endif

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
#ifdef TASK_DEBUG
    if(t == NULL)
      panic("No task on queue %d", which);
#endif
    TAILQ_REMOVE(&readyqueue[which], t, t_ready_link);

    if(TAILQ_FIRST(&readyqueue[which]) == NULL) {
      active_queues &= ~(1 << t->t_prio);
    }
    assert(t->t_state == TASK_STATE_RUNNING);
  }

#ifdef TASK_DEBUG
  if(task_trace) {
    uint32_t *sp = t->t_sp;
    printf("Switch from %p:%s [sp:%p] to %p:%s [sp:%p PC:%x]\n",
           curtask, curtask->t_name, curtask->t_sp,
           t, t->t_name, t->t_sp, sp[14]);

  }
#endif

  cpu->sched.current = t;
  irq_permit(s);

#ifdef TASK_ACCOUNTING
  t->t_cycle_enter = cpu_cycle_counter();
  t->t_ctx_switches_acc++;
#endif

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

  curtask->t_state = TASK_STATE_ZOMBIE;

  if(curtask->t_flags & TASK_DETACHED) {
    cond_signal(&task_mgmt_cond);
  } else {
    task_wakeup(&join_wait, 1);
  }
  irq_permit(s);

  schedule();
  irq_lower();
  while(1) {
  }
}


static void
task_destroy(task_t *t)
{
  SLIST_REMOVE(&alltasks, t, task, t_global_link);
  free(t->t_sp_bottom);
}

void *
task_join(task_t *t)
{
  int s = irq_forbid(IRQ_LEVEL_SWITCH);

  assert((t->t_flags & TASK_DETACHED) == 0);

  while(t->t_state != TASK_STATE_ZOMBIE)
    task_sleep(&join_wait);

  irq_permit(s);

  mutex_lock(&alltasks_mutex);
  task_destroy(t);
  mutex_unlock(&alltasks_mutex);

  return NULL;
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

  void *sp_bottom = xalloc(stack_size + fpu_ctx_size + sizeof(task_t),
                           CPU_STACK_ALIGNMENT,
                           flags & TASK_DMA_STACK ? MEM_TYPE_DMA : 0);

  void *sp = sp_bottom + stack_size;
  task_t *t = sp + fpu_ctx_size;
  strlcpy(t->t_name, name, sizeof(t->t_name));

  t->t_state = 0;
  t->t_prio = prio;
  t->t_flags = flags;

#ifdef TASK_ACCOUNTING
  t->t_cycle_acc = 0;
  t->t_load = 0;
  t->t_ctx_switches = 0;
  t->t_ctx_switches_acc = 0;
#endif

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
  TAILQ_INSERT_TAIL(&readyqueue[t->t_prio], t, t_ready_link);
  active_queues |= 1 << t->t_prio;
  irq_permit(s);

  mutex_lock(&alltasks_mutex);
  SLIST_INSERT_HEAD(&alltasks, t, t_global_link);
  mutex_unlock(&alltasks_mutex);

  schedule();
  return t;
}


void
task_wakeup(task_waitable_t *waitable, int all)
{
  int s = irq_forbid(IRQ_LEVEL_SCHED);
  task_t *t;
  while((t = LIST_FIRST(&waitable->list)) != NULL) {
    assert(t->t_state == TASK_STATE_SLEEPING);
    LIST_REMOVE(t, t_wait_link);
    t->t_state = TASK_STATE_RUNNING;
    if(t != task_current())
      readyqueue_insert(t, "wakeup");
    schedule();
    if(!all)
      break;
  }
  irq_permit(s);
}

static int
task_prio_cmp(const task_t *a, const task_t *b)
{
  // Higher prio value -> earlier in list
  // Maintain insert order for equal priorities
  return a->t_prio <= b->t_prio;
}



static void __attribute__((noinline))
task_insert_wait_list(task_waitable_t *waitable, task_t *t)
{
  LIST_INSERT_SORTED(&waitable->list, t, t_wait_link, task_prio_cmp);
}









static void
task_sleep_abs_sched_locked_timeout(void *opaque)
{
  task_t *t = opaque;

  const int s = irq_forbid(IRQ_LEVEL_SCHED);

  if(t->t_state == TASK_STATE_SLEEPING) {

    LIST_REMOVE(t, t_wait_link);

    t->t_state = TASK_STATE_RUNNING;
    if(t != task_current())
      readyqueue_insert(t, "sleep-timo");
    schedule();
  }
  irq_permit(s);
}


static int
task_sleep_abs_sched_locked(task_waitable_t *waitable,
                            int64_t deadline, int flags)
{
  task_t *const curtask = task_current();

  timer_t timer;

  assert(waitable != NULL);
  assert(curtask->t_state == TASK_STATE_RUNNING);
  curtask->t_state = TASK_STATE_SLEEPING;

#ifdef TASK_WCHAN
  curtask->t_wchan = waitable->name ?: __FUNCTION__;
#endif

#ifdef TASK_DEBUG
  if(task_is_on_readyqueue(curtask)) {
    panic("%s: Task %p is on readyqueue",
          __FUNCTION__, curtask);
  }
#endif

  timer.t_cb = task_sleep_abs_sched_locked_timeout;
  timer.t_opaque = curtask;
  timer.t_expire = 0;
  timer.t_name = curtask->t_name;
  timer_arm_abs(&timer, deadline, flags);

  task_insert_wait_list(waitable, curtask);

  while(curtask->t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }

  return timer_disarm(&timer);
}


static void
task_sleep_sched_locked(task_waitable_t *waitable)
{
  task_t *const curtask = task_current();

  assert(waitable != NULL);
  assert(curtask->t_state == TASK_STATE_RUNNING);
  curtask->t_state = TASK_STATE_SLEEPING;
#ifdef TASK_WCHAN
  curtask->t_wchan = waitable->name ?: __FUNCTION__;
#endif

#ifdef TASK_DEBUG
  if(task_is_on_readyqueue(curtask)) {
    panic("%s: Task %p is on readyqueue",
          __FUNCTION__, curtask);
  }
#endif

  task_insert_wait_list(waitable, curtask);

  while(curtask->t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }
}




void
task_sleep(task_waitable_t *waitable)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  task_sleep_sched_locked(waitable);
  irq_permit(s);
}


int
task_sleep_delta(task_waitable_t *waitable, int useconds, int flags)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  const int64_t deadline = clock_get_irq_blocked() + useconds;
  const int r = task_sleep_abs_sched_locked(waitable, deadline, flags);
  irq_permit(s);
  return r;
}

int
task_sleep_deadline(task_waitable_t *waitable, int64_t deadline, int flags)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  const int r = task_sleep_abs_sched_locked(waitable, deadline, flags);
  irq_permit(s);
  return r;
}



static void
task_sleep_until_timeout(void *opaque)
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
task_sleep_until(uint64_t deadline, int flags, const char *wchan)
{
  task_t *const curtask = task_current();

  timer_t timer;

  assert(curtask->t_state == TASK_STATE_RUNNING);
  curtask->t_state = TASK_STATE_SLEEPING;
#ifdef TASK_WCHAN
  curtask->t_wchan = wchan;
#endif

  timer.t_cb = task_sleep_until_timeout;
  timer.t_opaque = curtask;
  timer.t_expire = 0;
  timer.t_name = curtask->t_name;
  timer_arm_abs(&timer, deadline, flags);

  while(curtask->t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }
}


void
sleep_until(uint64_t deadline)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  task_sleep_until(deadline, 0, "sleep_until");
  irq_permit(s);
}

void
sleep_until_hr(uint64_t deadline)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  task_sleep_until(deadline, TIMER_HIGHRES, "sleep_until_hr");
  irq_permit(s);
}


void
usleep(unsigned int useconds)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  task_sleep_until(clock_get_irq_blocked() + useconds, 0, "usleep");
  irq_permit(s);
}

void
usleep_hr(unsigned int useconds)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  task_sleep_until(clock_get_irq_blocked() + useconds, TIMER_HIGHRES,
                   "usleep_hr");
  irq_permit(s);
}


void
sleep(unsigned int sec)
{
  usleep(sec * 1000000);
}


static void
mutex_lock_sched_locked(mutex_t *m, task_t *curtask)
{
  while(m->owner != NULL) {
    assert(m->owner != curtask);

#ifdef TASK_DEBUG
    if(task_is_on_readyqueue(curtask)) {
      panic("%s: Task %p is on readyqueue",
            __FUNCTION__, curtask);
    }

    if(task_is_on_list(curtask, &m->waiters.list)) {
      panic("%s: Task %p is already on wait queue",
            __FUNCTION__, curtask);
    }

    if(task_trace) {
      printf("Task %p:%s waiting owner %p:%s\n",
             curtask, curtask->t_name,
             m->owner, m->owner->t_name);
    }
#endif

    if(curtask->t_state != TASK_STATE_SLEEPING) {
      curtask->t_state = TASK_STATE_SLEEPING;
      task_insert_wait_list(&m->waiters, curtask);
    }

    schedule();
    irq_permit(irq_lower());
  }
  m->owner = curtask;
}


void
mutex_lock(mutex_t *m)
{
  task_t *const curtask = task_current();

  if(cpu_mutex_lock_fast(m, curtask))
    return;

  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  mutex_lock_sched_locked(m, curtask);
  irq_permit(s);
}


int
mutex_trylock(mutex_t *m)
{
  return !cpu_mutex_lock_fast(m, task_current());
}



static void
mutex_unlock_sched_locked(mutex_t *m)
{
  task_t *cur = task_current();
  assert(m->owner == cur);
  m->owner = NULL;

  task_t *t = LIST_FIRST(&m->waiters.list);
  if(t != NULL) {
    assert(t != cur);
    LIST_REMOVE(t, t_wait_link);
    t->t_state = TASK_STATE_RUNNING;
    readyqueue_insert(t, "mutex_unlock");
    if(t->t_prio >= cur->t_prio)
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
cond_signal(cond_t *c)
{
  task_wakeup(c, 0);
}


void
cond_broadcast(cond_t *c)
{
  task_wakeup(c, 1);
}


void
cond_wait(cond_t *c, mutex_t *m)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  mutex_unlock_sched_locked(m);
  task_sleep_sched_locked(c);
  mutex_lock_sched_locked(m, task_current());
  irq_permit(s);
}


int
cond_wait_timeout(cond_t *c, mutex_t *m, uint64_t deadline, int flags)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  mutex_unlock_sched_locked(m);
  int r = task_sleep_abs_sched_locked(c, deadline, flags);
  mutex_lock_sched_locked(m, task_current());
  irq_permit(s);
  return r;
}


static void __attribute__((constructor(101)))
task_init_early(void)
{
  for(int i = 0; i < TASK_PRIOS; i++)
    TAILQ_INIT(&readyqueue[i]);
}


static void __attribute__((noreturn))
task_mgmt_thread(void *arg)
{
#ifdef TASK_ACCOUNTING
  int64_t ts = clock_get();
  uint32_t prev_cc = cpu_cycle_counter();
#endif

  mutex_lock(&alltasks_mutex);

  while(1) {


#ifdef TASK_ACCOUNTING
    ts += 1000000;
    int do_accounting =
      cond_wait_timeout(&task_mgmt_cond, &alltasks_mutex, ts, 0);
    uint32_t cc = cpu_cycle_counter();
    uint32_t cc_delta = (cc - prev_cc) / 10000;
    prev_cc = cc;
#else
    cond_wait(&task_mgmt_cond, &alltasks_mutex);
#endif

    int s = irq_forbid(IRQ_LEVEL_SWITCH);
    task_t *t, *n;
    for(t = SLIST_FIRST(&alltasks); t != NULL; t = n) {
      n = SLIST_NEXT(t, t_global_link);

      if(t->t_state == TASK_STATE_ZOMBIE && t->t_flags & TASK_DETACHED) {
        task_destroy(t);
        continue;
      }

#ifdef TASK_ACCOUNTING
      if(do_accounting) {
        t->t_load = t->t_cycle_acc / cc_delta;
        t->t_cycle_acc = 0;
        t->t_ctx_switches = t->t_ctx_switches_acc;
        t->t_ctx_switches_acc = 0;
      }
#endif
    }
    irq_permit(s);
  }
}

static void __attribute__((constructor(900)))
task_init_late(void)
{
  task_create((void *)task_mgmt_thread, NULL, 256, "taskmgmt", 0, 0);
}



static int
cmd_ps(cli_t *cli, int argc, char **argv)
{
  task_t *t;
  cli_printf(cli, " Name           Stack      Sp         Pri Sta CtxSwch Load\n");
  SLIST_FOREACH(t, &alltasks, t_global_link) {
    cli_printf(cli, " %-14s %p %p %3d %c%c%c "
#ifdef TASK_ACCOUNTING
               "%-6d %3d.%-2d "
#endif
#ifdef TASK_WCHAN
               "%s"
#endif
               "\n",
               t->t_name, t->t_sp_bottom, t->t_sp,
               t->t_prio,
               "RSZ"[t->t_state],
               t->t_fpuctx ? 'F' : ' ',
               (t->t_flags & TASK_DETACHED) ? 'd' : ' '
#ifdef TASK_ACCOUNTING
               ,t->t_ctx_switches,
               t->t_load / 100,
               t->t_load % 100
#endif
#ifdef TASK_WCHAN
               ,t->t_state == TASK_STATE_SLEEPING ? t->t_wchan : ""
#endif
               );
  }
  return 0;
}

CLI_CMD_DEF("ps", cmd_ps);
