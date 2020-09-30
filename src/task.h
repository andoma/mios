#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>

#include "timer.h"

#define TASK_ACCOUNTING

TAILQ_HEAD(task_queue, task);

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
  TAILQ_ENTRY(task) t_link;
  void *t_sp_bottom;
  void *t_sp;
  void *t_fpuctx; // If NULL, task is not allowed to use FPU

#ifdef TASK_ACCOUNTING
  uint32_t t_cycle_enter;
  uint32_t t_cycle_acc;
  uint32_t t_load;
  uint32_t t_ctx_switches_acc;
  uint32_t t_ctx_switches;
#endif

  SLIST_ENTRY(task) t_global_link;
  char t_name[14];
  uint8_t t_prio;
  uint8_t t_state;
} task_t;

typedef struct sched_cpu {

  task_t idle;
  task_t *current;
  task_t *current_fpu;

} sched_cpu_t;


typedef struct mutex {
  struct task_queue waiters;
  struct task *owner;
} mutex_t;

typedef struct cond {
  struct task_queue waiters;
} cond_t;

void task_init_cpu(sched_cpu_t *sc, const char *cpu_name, void *sp_bottom);

#define TASK_FPU 0x1

task_t *task_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
                    const char *name, int flags, unsigned int prio);

void task_wakeup(struct task_queue *waitable, int all);

int task_sleep(struct task_queue *waitable, int useconds);

task_t *task_current(void);

#define MUTEX_INITIALIZER(self) { { NULL, &(self).waiters.tqh_first}}

void mutex_init(mutex_t *m);

void mutex_lock(mutex_t *m);

void mutex_unlock(mutex_t *m);


void cond_init(cond_t *c);

void cond_signal(cond_t *c);

void cond_broadcast(cond_t *c);

void cond_wait(cond_t *c, mutex_t *m);

int cond_wait_timeout(cond_t *c, mutex_t *m, uint64_t deadline);
