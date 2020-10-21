#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>

#include "timer.h"

#define TASK_ACCOUNTING

LIST_HEAD(task_list, task);

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
  void *t_fpuctx; // If NULL, task is not allowed to use FPU

#ifdef TASK_WCHAN
  const char *t_wchan; // Valid when state == SLEEPING
#endif

#ifdef TASK_ACCOUNTING
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

  task_t idle;
  task_t *current;
  task_t *current_fpu;

} sched_cpu_t;



typedef struct {
  struct task_list list;
#ifdef TASK_WCHAN
  const char *name;
#endif
} task_waitable_t;

typedef struct mutex {
  struct task *owner;
  task_waitable_t waiters;
} mutex_t;

typedef task_waitable_t cond_t;

#ifdef TASK_WCHAN
#define task_waitable_init(t, n) do { LIST_INIT(&(t)->list); (t)->name = n; } while(0)

#else
#define task_waitable_init(t, n) LIST_INIT(&(t)->list)

#endif


void task_init_cpu(sched_cpu_t *sc, const char *cpu_name, void *sp_bottom);

#define TASK_FPU       0x1
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

#ifdef TASK_WCHAN
#define MUTEX_INITIALIZER(n) { .waiters = {.name = (n)}}
#else
#define MUTEX_INITIALIZER(n) {}
#endif


inline void  __attribute__((always_inline))
mutex_init(mutex_t *m, const char *name)
{
  LIST_INIT(&m->waiters.list);
#ifdef TASK_WCHAN
  m->waiters.name = name;
#endif
  m->owner = NULL;
}

void mutex_lock(mutex_t *m);

void mutex_unlock(mutex_t *m);


#ifdef TASK_WCHAN
#define COND_INITIALIZER(n) {.name = (n)}
#else
#define COND_INITIALIZER(n) {}
#endif

inline void  __attribute__((always_inline))
cond_init(cond_t *c, const char *name)
{
  LIST_INIT(&c->list);
#ifdef TASK_WCHAN
  c->name = name;
#endif
}

void cond_signal(cond_t *c);

void cond_broadcast(cond_t *c);

void cond_wait(cond_t *c, mutex_t *m);

int cond_wait_timeout(cond_t *c, mutex_t *m, uint64_t deadline, int flags)
  __attribute__((warn_unused_result));

