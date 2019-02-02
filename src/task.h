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
  const char *t_name;
  uint8_t t_state;
  struct task_queue *t_waitable;
  void *t_psp;
  uint8_t t_stack[0];
} task_t;

extern task_t *curtask;


task_t *task_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
                    const char *name);

void task_wakeup(struct task_queue *waitable, int all);

void task_sleep(struct task_queue *waitable, int ticks);
