#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>

#include "timer.h"

TAILQ_HEAD(task_queue, task);
LIST_HEAD(task_list, task);

#define TASK_PRIOS 32
#define TASK_PRIO_MASK (TASK_PRIOS - 1)


#define TASK_STATE_RUNNING  0
#define TASK_STATE_SLEEPING 1
#define TASK_STATE_ZOMBIE   2


/*
 * Per task memory block
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

typedef struct task {
  union {
    TAILQ_ENTRY(task) t_ready_link;
    LIST_ENTRY(task) t_wait_link;
  };

  void *t_sp_bottom;
  void *t_sp;
#ifdef HAVE_FPU
  void *t_fpuctx; // If NULL, task is not allowed to use FPU
#endif

#ifdef ENABLE_TASK_WCHAN
  const char *t_wchan; // Valid when state == SLEEPING
#endif

#ifdef ENABLE_TASK_ACCOUNTING
  uint32_t t_cycle_enter;
  uint32_t t_cycle_acc;
  uint32_t t_load;
  uint32_t t_ctx_switches_acc;
  uint32_t t_ctx_switches;
#endif

  SLIST_ENTRY(task) t_global_link;
  char t_name[13];
  uint8_t t_flags;
  uint8_t t_prio;
  uint8_t t_state;
} task_t;

typedef struct sched_cpu {
  task_t *current;
  task_t *idle;
  uint32_t active_queues;
  struct task_queue readyqueue[TASK_PRIOS];

#ifdef HAVE_FPU
  task_t *current_fpu;
#endif

} sched_cpu_t;


void sched_cpu_init(sched_cpu_t *sc, task_t *idle);

typedef struct {
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
  struct task *owner;
  task_waitable_t waiters;
} mutex_t;

typedef task_waitable_t cond_t;

void task_init_cpu(sched_cpu_t *sc, const char *cpu_name, void *sp_bottom);

#ifdef HAVE_FPU
#define TASK_FPU       0x1
#endif

#define TASK_DMA_STACK 0x2
#define TASK_DETACHED  0x4


task_t *task_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
                    const char *name, int flags, unsigned int prio);

void task_wakeup(task_waitable_t *waitable, int all);

void task_sleep(task_waitable_t *waitable);

int task_sleep_deadline(task_waitable_t *waitable, int64_t deadline, int flags)
  __attribute__((warn_unused_result));

int task_sleep_delta(task_waitable_t *waitable, int useconds, int flags)
  __attribute__((warn_unused_result));


task_t *task_current(void);

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
  m->owner = NULL;
}

void mutex_lock(mutex_t *m);

int mutex_trylock(mutex_t *m); // Return 0 if locked

void mutex_unlock(mutex_t *m);


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

int cond_wait_timeout(cond_t *c, mutex_t *m, uint64_t deadline, int flags)
  __attribute__((warn_unused_result));

