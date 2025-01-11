#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>

#include "error.h"

STAILQ_HEAD(task_queue, task);
LIST_HEAD(task_list, task);

#define TASK_PRIOS 32
#define TASK_PRIO_MASK (TASK_PRIOS - 1)


#define TASK_STATE_NONE     0
#define TASK_STATE_RUNNING  1
#define TASK_STATE_READY    2
#define TASK_STATE_SLEEPING 3
#define TASK_STATE_ZOMBIE   4
#define TASK_STATE_MUXED_SLEEP 5

typedef struct task {
  union {
    struct {
      STAILQ_ENTRY(task) t_ready_link;
      void (*t_run)(struct task *t);
    };
    LIST_ENTRY(task) t_wait_link;
  };

  uint8_t t_flags;
  uint8_t t_prio;
  uint8_t t_state;
  uint8_t t_index;
} task_t;

void task_run(task_t *t);

 /*
 * Per thread memory block
 *
 * ----------------- <- sp_bottom
 * | REDZONE       |
 * -----------------
 * | Normal stack  |
 * ----------------- <- initial sp points here
 * | FPU save area | (Optional)
 * -----------------
 * | struct task   |
 * -----------------
 *
 */


typedef struct thread {
  task_t t_task;

  void *t_sp_bottom;
  void *t_sp;
#ifdef HAVE_FPU
  void *t_fpuctx; // If NULL, task is not allowed to use FPU
#endif

#ifdef ENABLE_TASK_WCHAN
  const char *t_wchan; // Valid when state == SLEEPING
#endif

  SLIST_ENTRY(thread) t_global_link;
  struct stream *t_stream;

  char t_name[11];
  uint8_t t_refcount;

#ifdef ENABLE_TASK_ACCOUNTING
  uint32_t t_cycle_enter;
  uint32_t t_cycle_acc;
  uint32_t t_ctx_switches_acc;
  uint32_t t_ctx_switches;
  uint16_t t_load;
  uint16_t t_stacksize;
#endif

} thread_t;

typedef struct sched_cpu {
  thread_t *current;
  task_t *idle;
  uint32_t active_queues;
  struct task_queue readyqueue[TASK_PRIOS];

#ifdef HAVE_FPU
  thread_t *current_fpu;
#endif

} sched_cpu_t;


void sched_cpu_init(sched_cpu_t *sc, thread_t *idle);

typedef struct task_waitable {
  struct task_list list;
#ifdef ENABLE_TASK_WCHAN
  const char *name;
#endif
} task_waitable_t;


#ifdef ENABLE_TASK_WCHAN
#define task_waitable_init(t, n) do { LIST_INIT(&(t)->list); (t)->name = n; } while(0)
#define WAITABLE_INITIALIZER(n) { .name = (n)}
#else
#define task_waitable_init(t, n) LIST_INIT(&(t)->list)
#define WAITABLE_INITIALIZER(n) {}

#endif


typedef struct mutex {
  union {
    task_waitable_t waiters;
    intptr_t lock;
  };
} mutex_t;

typedef task_waitable_t cond_t;

void thread_init_cpu(sched_cpu_t *sc, const char *cpu_name, void *sp_bottom);


// Flag bits 0-7 is stored in task->t_flags
#define TASK_THREAD    0x1  // A full thread with stack
#define TASK_DETACHED  0x2  // Should be auto-joined by system on thread_exit

// Remaining flags are only used during thread_create
#define TASK_NO_FPU       0x100
#define TASK_NO_DMA_STACK 0x200

thread_t *thread_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
                        const char *name, int flags, unsigned int prio);

void thread_exit(void *ret) __attribute__((noreturn));

void *thread_join(thread_t *t);

void task_wakeup(task_waitable_t *waitable, int all);

// Use if you have irq_forbid(IRQ_LEVEL_SCHED)
void task_wakeup_sched_locked(task_waitable_t *waitable, int all);

void task_sleep(task_waitable_t *waitable);

// Use if you have irq_forbid(IRQ_LEVEL_SCHED)
void task_sleep_sched_locked(task_waitable_t *waitable);

// Returns 1 if deadline expired
int task_sleep_deadline(task_waitable_t *waitable, int64_t deadline)
  __attribute__((warn_unused_result));

// Returns 1 if deadline expired
int task_sleep_delta(task_waitable_t *waitable, int useconds)
  __attribute__((warn_unused_result));

thread_t *thread_current(void);

#ifdef ENABLE_TASK_WCHAN
#define MUTEX_INITIALIZER(n) { .waiters = {.name = (n)}}
#else
#define MUTEX_INITIALIZER(n) {}
#endif


inline void  __attribute__((always_inline))
mutex_init(mutex_t *m, const char *name)
{
  LIST_INIT(&m->waiters.list);
#ifdef ENABLE_TASK_WCHAN
  m->waiters.name = name;
#endif
}

void mutex_lock_slow(mutex_t *m);

inline void  __attribute__((always_inline))
mutex_lock(mutex_t *m)
{
  if(__atomic_always_lock_free(sizeof(intptr_t), 0)) {
    intptr_t expected = 0;
    if(__builtin_expect(__atomic_compare_exchange_n(&m->lock, &expected, 1, 1,
                                                    __ATOMIC_SEQ_CST,
                                                    __ATOMIC_RELAXED), 1))
      return;
  }
  mutex_lock_slow(m);
}


// Return 0 if locked
int mutex_trylock_slow(mutex_t *m);

// Return 0 if locked
inline int  __attribute__((always_inline))
mutex_trylock(mutex_t *m)
{
  if(__atomic_always_lock_free(sizeof(intptr_t), 0)) {
    intptr_t expected = 0;
    if(__builtin_expect(__atomic_compare_exchange_n(&m->lock, &expected, 1, 1,
                                                    __ATOMIC_SEQ_CST,
                                                    __ATOMIC_RELAXED), 1))
      return 0;
  }
  return mutex_trylock_slow(m);
}


void mutex_unlock_slow(mutex_t *m);

inline void  __attribute__((always_inline))
mutex_unlock(mutex_t *m)
{
  if(__atomic_always_lock_free(sizeof(intptr_t), 0)) {
    intptr_t expected = 1;
    if(__builtin_expect(__atomic_compare_exchange_n(&m->lock, &expected, 0, 1,
                                                    __ATOMIC_SEQ_CST,
                                                    __ATOMIC_RELAXED), 1))
      return;
  }
  mutex_unlock_slow(m);
}


#ifdef ENABLE_TASK_WCHAN
#define COND_INITIALIZER(n) {.name = (n)}
#else
#define COND_INITIALIZER(n) {}
#endif

inline void  __attribute__((always_inline))
cond_init(cond_t *c, const char *name)
{
  LIST_INIT(&c->list);
#ifdef ENABLE_TASK_WCHAN
  c->name = name;
#endif
}

void cond_signal(cond_t *c);

void cond_broadcast(cond_t *c);

void cond_wait(cond_t *c, mutex_t *m);

int cond_wait_timeout(cond_t *c, mutex_t *m, uint64_t deadline)
  __attribute__((warn_unused_result));

// Helper for constructing a thread used for cli/shell activities
error_t thread_create_shell(void *(*entry)(void *arg), void *arg,
                            const char *name, struct stream *log_st);


typedef struct waitmux {
  task_waitable_t wm_waitable;
  int wm_which;
  task_t wm_tasks[0];
} waitmux_t;



/*
 * SoftIRQs can be raised from any context (even > IRQ_LEVEL_SCHED)
 * as opposed to task_run() which can only be called from IRQ_LEVEL_SCHED
 * and below
 */


#ifndef NUM_SOFTIRQ
#define NUM_SOFTIRQ 0
#endif

#if NUM_SOFTIRQ > 0

void softirq_raise(uint32_t id);

uint32_t softirq_alloc(void (*fn)(void *arg), void *arg);

#endif
