#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>

#include "timer.h"

TAILQ_HEAD(task_queue, task);

#define TASK_STATE_RUNNING  0
#define TASK_STATE_SLEEPING 1
#define TASK_STATE_ZOMBIE   2

typedef struct task {
  TAILQ_ENTRY(task) t_link;
  void *t_sp;
  void *t_fpuctx; // If NULL, task is not allowed to use FPU
  char t_name[16];
  uint8_t t_state;
  uint8_t t_stack[0]  __attribute__ ((aligned (8)));
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

void task_init_cpu(sched_cpu_t *sc, const char *cpu_name);

#define TASK_FPU 0x1

task_t *task_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
                    const char *name, int flags);

void task_wakeup(struct task_queue *waitable, int all);

int task_sleep(struct task_queue *waitable, int ticks);

task_t *task_current(void);


void mutex_init(mutex_t *m);

void mutex_lock(mutex_t *m);

void mutex_unlock(mutex_t *m);


void cond_init(cond_t *c);

void cond_signal(cond_t *c);

void cond_broadcast(cond_t *c);

void cond_wait(cond_t *c, mutex_t *m);
