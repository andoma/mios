#ifndef ENABLE_TASK_DEBUG
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
#include <mios/timer.h>

#include "sys.h"
#include "irq.h"
#include "cpu.h"

SLIST_HEAD(thread_slist, thread);

static struct thread_slist allthreads;

static task_waitable_t join_wait;

#if NUM_SOFTIRQ > 0

static struct {
  void (*fn)(void *arg);
  void *arg;
} softirq[NUM_SOFTIRQ];

static uint32_t softirq_pending;

uint32_t
softirq_alloc(void (*fn)(void *arg), void *arg)
{
  int q = irq_forbid(IRQ_LEVEL_SWITCH);

  for(uint32_t i = 0; i < NUM_SOFTIRQ; i++) {
    if(softirq[i].fn == NULL) {
      softirq[i].fn = fn;
      softirq[i].arg = arg;
      irq_permit(q);
      return i;
    }
  }

  panic("Out of softirq");
}

void
softirq_raise(uint32_t id)
{
  __atomic_or_fetch(&softirq_pending, (1 << id), __ATOMIC_SEQ_CST);
  schedule();
}

#endif


inline thread_t *
thread_current(void)
{
  return curcpu()->sched.current;
}

inline task_t *
task_current(void)
{
  return &thread_current()->t_task;
}

#ifdef ENABLE_TASK_DEBUG

static int
task_is_on_queue(task_t *t, struct task_queue *q)
{
  task_t *x;
  STAILQ_FOREACH(x, q, t_ready_link) {
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
task_is_on_readyqueue(cpu_t *cpu, task_t *t)
{
  for(int i = 0; i < TASK_PRIOS; i++) {
    if(task_is_on_queue(t, &cpu->sched.readyqueue[i]))
      return 1;
  }
  return 0;
}
#endif

static void
readyqueue_insert(cpu_t *cpu, task_t *t, const char *whom)
{
#ifdef ENABLE_TASK_DEBUG
  if(task_is_on_readyqueue(cpu, t)) {
    panic("%s: Inserting task %p on readyqueue but it's already there",
          whom, t);
  }
  if(STAILQ_FIRST(&cpu->sched.readyqueue[t->t_prio]) == NULL) {
    assert(!(cpu->sched.active_queues & 1 << t->t_prio));
  } else {
    assert(cpu->sched.active_queues & 1 << t->t_prio);
  }
  assert(t->t_state != TASK_STATE_READY);
#endif
  STAILQ_INSERT_TAIL(&cpu->sched.readyqueue[t->t_prio], t, t_ready_link);
  cpu->sched.active_queues |= 1 << t->t_prio;
  t->t_state = TASK_STATE_READY;
}


void *
task_switch(void *cur_sp)
{
  cpu_t *cpu = curcpu();
  thread_t *const curthread = thread_current();
  curthread->t_sp = cur_sp;

#ifdef ENABLE_TASK_ACCOUNTING
  curthread->t_cycle_acc += cpu_cycle_counter() - curthread->t_cycle_enter;
#endif


#if NUM_SOFTIRQ > 0

  uint32_t pending = __atomic_fetch_and(&softirq_pending, 0, __ATOMIC_SEQ_CST);

  while(1) {
    int which = __builtin_clz(pending);
    if(which == 32)
      break;
    which = 31 - which;
    softirq[which].fn(softirq[which].arg);
    pending &= ~(1 << which);
  }

#endif

  int q = irq_forbid(IRQ_LEVEL_SCHED);

  thread_t *t;

  while(1) {

    if(curthread->t_task.t_state == TASK_STATE_RUNNING) {
      // Current thread should be running, re-insert in readyqueue
      readyqueue_insert(cpu, &curthread->t_task, "task_switch");
    }

    task_t *task;
    int which = __builtin_clz(cpu->sched.active_queues);

    if(which == 32) {

      task = cpu->sched.idle;

    } else {

      which = 31 - which;
      task = STAILQ_FIRST(&cpu->sched.readyqueue[which]);
#ifdef ENABLE_TASK_DEBUG
      if(task == NULL)
        panic("No task on queue %d", which);
      assert(task->t_state == TASK_STATE_READY);
#endif
      STAILQ_REMOVE_HEAD(&cpu->sched.readyqueue[which], t_ready_link);

      if(STAILQ_FIRST(&cpu->sched.readyqueue[which]) == NULL) {
        cpu->sched.active_queues &= ~(1 << task->t_prio);
      }
    }

    if(likely(task->t_flags & TASK_THREAD)) {
      t = (thread_t *)task;
      break;
    }

    task->t_state = TASK_STATE_NONE;
    irq_permit(q);
    task->t_run(task);
    q = irq_forbid(IRQ_LEVEL_SCHED);
  }

  t->t_task.t_state = TASK_STATE_RUNNING;
  cpu->sched.current = t;
  irq_permit(q);

#ifdef ENABLE_TASK_ACCOUNTING
  t->t_cycle_enter = cpu_cycle_counter();
  t->t_ctx_switches_acc++;
#endif

  cpu_stack_redzone(t);

#ifdef HAVE_FPU
  cpu_fpu_enable(cpu->sched.current_fpu == t);
#endif

  return t->t_sp;
}


void
task_run(task_t *t)
{
  int q = irq_forbid(IRQ_LEVEL_SCHED);
  if(likely(t->t_state == TASK_STATE_NONE)) {
    readyqueue_insert(curcpu(), t, "task_run");
  }
  irq_permit(q);
  schedule();
}


static void
thread_retain(thread_t *t)
{
  t->t_refcount++;
}



static void
thread_exit2(task_t *ta)
{
  thread_t *t = (thread_t *)ta;
  if(free_try(t->t_sp_bottom)) {
    // Failed to free (malloc mutex held, keep trying)
    readyqueue_insert(curcpu(), ta, "zombie");
  }
}

static void
thread_release(thread_t *t)
{
  t->t_refcount--;
  if(t->t_refcount)
    return;

  cpu_t *cpu = curcpu();
  t->t_task.t_flags &= ~TASK_THREAD;
  t->t_task.t_run = thread_exit2;
  t->t_task.t_prio = 1;
  readyqueue_insert(cpu, &t->t_task, "zombie");
}

void
thread_exit(void *arg)
{
  thread_t *curthread = thread_current();

  int s = irq_forbid(IRQ_LEVEL_SWITCH);
  SLIST_REMOVE(&allthreads, curthread, thread, t_global_link);

#ifdef HAVE_FPU
  cpu_t *cpu = curcpu();
  if(cpu->sched.current_fpu == curthread) {
    cpu->sched.current_fpu = NULL;
    cpu_fpu_enable(0);
  }
#endif
  if(curthread->t_task.t_flags & TASK_DETACHED) {
    thread_release(curthread);
  } else {
    curthread->t_task.t_state = TASK_STATE_ZOMBIE;
    task_wakeup(&join_wait, 1);
  }
  irq_permit(s);

  schedule();
  irq_lower();
  while(1) {
  }
}


void *
thread_join(thread_t *t)
{
  int s = irq_forbid(IRQ_LEVEL_SWITCH);

  assert((t->t_task.t_flags & TASK_DETACHED) == 0);

  while(t->t_task.t_state != TASK_STATE_ZOMBIE)
    task_sleep(&join_wait);

  irq_permit(s);

  thread_release(t);
  return NULL;
}


thread_t *
thread_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
              const char *name, int flags, unsigned int prio)
{
  cpu_t *cpu = curcpu();

  prio &= TASK_PRIO_MASK;
  if(prio == 0)
    prio = 1;

  if(stack_size < MIN_STACK_SIZE)
    stack_size = MIN_STACK_SIZE;

  size_t fpu_ctx_size = 0;
#ifdef HAVE_FPU
  if(flags & TASK_FPU) {
    fpu_ctx_size += FPU_CTX_SIZE;
  }
#endif

  void *sp_bottom = xalloc(stack_size + fpu_ctx_size + sizeof(thread_t),
                           CPU_STACK_ALIGNMENT, MEM_MAY_FAIL |
                           (flags & TASK_DMA_STACK ? MEM_TYPE_DMA : 0));
  if(sp_bottom == NULL)
    return NULL;

  memset(sp_bottom, 0xbb, stack_size + fpu_ctx_size + sizeof(thread_t));
  void *sp = sp_bottom + stack_size;
  thread_t *t = sp + fpu_ctx_size;

#if 0
  printf("Created new task sp_bottom:%p sp:%p t:%p flags:0x%x %s\n",
         sp_bottom, sp, t, flags, name);
#endif

  strlcpy(t->t_name, name, sizeof(t->t_name));
  t->t_refcount = 1;

#ifdef ENABLE_TASK_ACCOUNTING
  t->t_cycle_acc = 0;
  t->t_load = 0;
  t->t_ctx_switches = 0;
  t->t_ctx_switches_acc = 0;
#endif

#ifdef HAVE_FPU
  if(flags & TASK_FPU) {
    t->t_fpuctx = sp_bottom + stack_size;
    cpu_fpu_ctx_init(t->t_fpuctx);
  } else {
    t->t_fpuctx = NULL;
  }
#endif

  t->t_sp = cpu_stack_init(sp, entry, arg, thread_exit);
  t->t_sp_bottom = sp_bottom;

  task_t *task = &t->t_task;
  task->t_state = TASK_STATE_READY;
  task->t_prio = prio;
  task->t_flags = flags | TASK_THREAD;
  task->t_run = NULL;

  int s = irq_forbid(IRQ_LEVEL_SCHED);
  STAILQ_INSERT_TAIL(&cpu->sched.readyqueue[task->t_prio], task, t_ready_link);
  cpu->sched.active_queues |= 1 << task->t_prio;
  SLIST_INSERT_HEAD(&allthreads, t, t_global_link);
  irq_permit(s);

  schedule();
  return t;
}


void
task_wakeup_sched_locked(task_waitable_t *waitable, int all)
{
  task_t *t;
  int do_sched = 0;
  while((t = LIST_FIRST(&waitable->list)) != NULL) {
    assert(t->t_state == TASK_STATE_SLEEPING);
    LIST_REMOVE(t, t_wait_link);
    cpu_t *cpu = curcpu();

    if(t != &cpu->sched.current->t_task) {
      if(t->t_prio >= cpu->sched.current->t_task.t_prio) {
        do_sched = 1;
      }
      readyqueue_insert(cpu, t, "wakeup");
    } else {
      do_sched = 1;
      t->t_state = TASK_STATE_RUNNING;
    }
    if(!all)
      break;
  }
  if(do_sched)
    schedule();
}


void
task_wakeup(task_waitable_t *waitable, int all)
{
  int s = irq_forbid(IRQ_LEVEL_SCHED);
  task_wakeup_sched_locked(waitable, all);
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
task_sleep_abs_sched_locked_timeout(void *opaque, uint64_t expire)
{
  task_t *t = opaque;

  const int s = irq_forbid(IRQ_LEVEL_SCHED);

  if(t->t_state == TASK_STATE_SLEEPING) {

    LIST_REMOVE(t, t_wait_link);

    cpu_t *cpu = curcpu();
    if(t != &cpu->sched.current->t_task) {
      readyqueue_insert(cpu, t, "sleep-timo");
    } else {
      t->t_state = TASK_STATE_RUNNING;
    }
    schedule();
  }
  irq_permit(s);
}


static int
task_sleep_abs_sched_locked(task_waitable_t *waitable,
                            int64_t deadline)
{
  thread_t *const cur = thread_current();

  timer_t timer;

  assert(waitable != NULL);
  assert(cur->t_task.t_state == TASK_STATE_RUNNING);
  cur->t_task.t_state = TASK_STATE_SLEEPING;

#ifdef ENABLE_TASK_WCHAN
  cur->t_wchan = waitable->name ?: __FUNCTION__;
#endif

#ifdef ENABLE_TASK_DEBUG
  if(task_is_on_readyqueue(curcpu(), &cur->t_task)) {
    panic("%s: Task %p is on readyqueue",
          __FUNCTION__, cur);
  }
#endif

  timer.t_cb = task_sleep_abs_sched_locked_timeout;
  timer.t_opaque = cur;
  timer.t_expire = 0;
  timer.t_name = cur->t_name;
  timer_arm_abs(&timer, deadline);

  task_insert_wait_list(waitable, &cur->t_task);

  while(cur->t_task.t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }

  return timer_disarm(&timer);
}


void
task_sleep_sched_locked(task_waitable_t *waitable)
{
  thread_t *const cur = thread_current();

  assert(waitable != NULL);
  assert(cur->t_task.t_state == TASK_STATE_RUNNING);
  cur->t_task.t_state = TASK_STATE_SLEEPING;
#ifdef ENABLE_TASK_WCHAN
  cur->t_wchan = waitable->name ?: __FUNCTION__;
#endif

#ifdef ENABLE_TASK_DEBUG
  if(task_is_on_readyqueue(curcpu(), &cur->t_task)) {
    panic("%s: Task %p is on readyqueue",
          __FUNCTION__, cur);
  }
#endif

  task_insert_wait_list(waitable, &cur->t_task);

  while(cur->t_task.t_state == TASK_STATE_SLEEPING) {
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
task_sleep_delta(task_waitable_t *waitable, int useconds)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  const int64_t deadline = clock_get_irq_blocked() + useconds;
  const int r = task_sleep_abs_sched_locked(waitable, deadline);
  irq_permit(s);
  return r;
}

int
task_sleep_deadline(task_waitable_t *waitable, int64_t deadline)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  const int r = task_sleep_abs_sched_locked(waitable, deadline);
  irq_permit(s);
  return r;
}



static void
task_sleep_until_timeout(void *opaque, uint64_t expire)
{
  task_t *t = opaque;
  const int s = irq_forbid(IRQ_LEVEL_SCHED);

  assert(t->t_state == TASK_STATE_SLEEPING);
  cpu_t *cpu = curcpu();
  if(t != &cpu->sched.current->t_task) {
    readyqueue_insert(cpu, t, "sleep-timo2");
  } else {
    t->t_state = TASK_STATE_RUNNING;
  }
  schedule();
  irq_permit(s);
}


static void
thread_sleep_until(uint64_t deadline, const char *wchan)
{
  thread_t *const cur = thread_current();

  timer_t timer;

  assert(cur->t_task.t_state == TASK_STATE_RUNNING);
  cur->t_task.t_state = TASK_STATE_SLEEPING;
#ifdef ENABLE_TASK_WCHAN
  cur->t_wchan = wchan;
#endif

  timer.t_cb = task_sleep_until_timeout;
  timer.t_opaque = cur;
  timer.t_expire = 0;
  timer.t_name = cur->t_name;
  timer_arm_abs(&timer, deadline);

  while(cur->t_task.t_state == TASK_STATE_SLEEPING) {
    schedule();
    irq_permit(irq_lower());
  }
}


void
sleep_until(uint64_t deadline)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  thread_sleep_until(deadline, "sleep_until");
  irq_permit(s);
}


void
usleep(unsigned int useconds)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  thread_sleep_until(clock_get_irq_blocked() + useconds, "usleep");
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
  while(m->lock & 1) {

#ifdef ENABLE_TASK_DEBUG
    if(task_is_on_readyqueue(curcpu(), curtask)) {
      panic("%s: Task %p is on readyqueue",
            __FUNCTION__, curtask);
    }
#endif

#ifdef ENABLE_TASK_DEBUG
    m->lock &= ~1;
    if(task_is_on_list(curtask, &m->waiters.list)) {
      panic("%s: Task %p is already on wait queue",
            __FUNCTION__, curtask);
    }
    m->lock |= 1;
#endif

    if(curtask->t_state != TASK_STATE_SLEEPING) {
      curtask->t_state = TASK_STATE_SLEEPING;
#ifdef ENABLE_TASK_WCHAN
      thread_t *curthread = (thread_t *)curtask;
      curthread->t_wchan = m->waiters.name ?: __FUNCTION__;
#endif
      // We need to clear the lockbit or list manipulation will fail
      // as they share the same memory address
      m->lock &= ~1;
      task_insert_wait_list(&m->waiters, curtask);
      m->lock |= 1;
    }

    schedule();
    irq_permit(irq_lower());
  }
  m->lock |= 1;
}


void
mutex_lock_slow(mutex_t *m)
{
  task_t *const curtask = task_current();

  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  mutex_lock_sched_locked(m, curtask);
  irq_permit(s);
}


int
mutex_trylock_slow(mutex_t *m)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  int r = m->lock & 1;
  if(!r) {
    m->lock |= 1;
  }
  irq_permit(s);
  return r;
}



static void
mutex_unlock_sched_locked(mutex_t *m)
{
  assert(m->lock != 0);

  m->lock &= ~1;

  task_t *t = LIST_FIRST(&m->waiters.list);
  if(t != NULL) {
    cpu_t *const cpu = curcpu();
    task_t *const cur = &cpu->sched.current->t_task;

    LIST_REMOVE(t, t_wait_link);
    readyqueue_insert(cpu, t, "mutex_unlock");
    if(t->t_prio >= cur->t_prio)
      schedule();
  }
}


void
mutex_unlock_slow(mutex_t *m)
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
cond_wait_timeout(cond_t *c, mutex_t *m, uint64_t deadline)
{
  const int s = irq_forbid(IRQ_LEVEL_SCHED);
  mutex_unlock_sched_locked(m);
  int r = task_sleep_abs_sched_locked(c, deadline);
  mutex_lock_sched_locked(m, task_current());
  irq_permit(s);
  return r;
}


void
sched_cpu_init(sched_cpu_t *sc, thread_t *idle)
{
  idle->t_task.t_flags = TASK_THREAD;
  sc->idle = &idle->t_task;
  sc->current = idle;
#ifdef HAVE_FPU
  sc->current_fpu = NULL;
#endif
  sc->active_queues = 0;

  for(int i = 0; i < TASK_PRIOS; i++)
    STAILQ_INIT(&sc->readyqueue[i]);
}



thread_t *
thread_get_next(thread_t *cur)
{
  thread_t *t;

  int q = irq_forbid(IRQ_LEVEL_SWITCH);

  if(cur == NULL) {
    t = SLIST_FIRST(&allthreads);
  } else {
    SLIST_FOREACH(t, &allthreads, t_global_link) {
      if(t == cur) {
        break;
      }
    }
    if(t)
      t = SLIST_NEXT(t, t_global_link);
  }

  if(t)
    thread_retain(t);
  if(cur)
    thread_release(cur);
  irq_permit(q);
  return t;
}

#ifdef ENABLE_TASK_ACCOUNTING

static uint32_t prev_cc;

static void
accounting_run(task_t *t_)
{
  thread_t *t;

  uint32_t cc = cpu_cycle_counter();
  uint32_t cc_delta = (cc - prev_cc) / 10000;
  prev_cc = cc;

  SLIST_FOREACH(t, &allthreads, t_global_link) {
    t->t_load = cc_delta ? t->t_cycle_acc / cc_delta : 0;
    t->t_cycle_acc = 0;
    t->t_ctx_switches = t->t_ctx_switches_acc;
    t->t_ctx_switches_acc = 0;
  }
}

static task_t accounting_task = {
  .t_run = accounting_run,
  .t_prio = 10,
};

static void accounting_timer_cb(void *opaque, uint64_t deadline);

static timer_t accounting_timer = {
  .t_cb = accounting_timer_cb,
  .t_opaque = &accounting_task,
  .t_name = "accounting",
};

static uint64_t accounting_next = 1000000;

static void
accounting_timer_cb(void *opaque, uint64_t deadline)
{
  accounting_next += 1000000;
  timer_arm_abs(&accounting_timer, accounting_next);
  task_run(opaque);
}

static void __attribute__((constructor(900)))
task_init_accounting(void)
{
  prev_cc = cpu_cycle_counter();
  timer_arm_abs(&accounting_timer, accounting_next);
}
#endif


static error_t
cmd_ps(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, " Name           Stack      Sp         Pri Sta CtxSwch Load\n");

  thread_t *t = NULL;
  while((t = thread_get_next(t)) != NULL) {
    cli_printf(cli, " %-14s %p %p %3d %c%c%c "
#ifdef ENABLE_TASK_ACCOUNTING
               "%-6d %3d.%-2d "
#endif
#ifdef ENABLE_TASK_WCHAN
               "%s"
#endif
               "\n",
               t->t_name, t->t_sp_bottom, t->t_sp,
               t->t_task.t_prio,
               "_RrSZ"[t->t_task.t_state],
#ifdef HAVE_FPU
               t->t_fpuctx ? 'F' : ' ',
#else
               ' ',
#endif
               (t->t_task.t_flags & TASK_DETACHED) ? 'd' : ' '
#ifdef ENABLE_TASK_ACCOUNTING
               ,t->t_ctx_switches,
               t->t_load / 100,
               t->t_load % 100
#endif
#ifdef ENABLE_TASK_WCHAN
               ,t->t_task.t_state == TASK_STATE_SLEEPING ? t->t_wchan : ""
#endif
               );
  }
  return 0;
}

CLI_CMD_DEF("ps", cmd_ps);


error_t
task_create_shell(void *(*entry)(void *arg), void *arg, const char *name,
                  size_t stack_size)
{
  int flags = TASK_DETACHED;
#ifdef HAVE_FPU
  flags |= TASK_FPU;
#endif
  if(!stack_size) {
    stack_size = 768;
#ifdef HAVE_FPU
    stack_size = 1024;
#endif
#ifdef MAIN_STACK_SIZE
    stack_size = MAIN_STACK_SIZE;
#endif
  }
  if(!thread_create(entry, arg, stack_size, name,  flags, 2))
    return ERR_NO_MEMORY;
  return 0;
}
