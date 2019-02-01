#pragma once
#include <stddef.h>
#include <sys/queue.h>

#include "timer.h"

TAILQ_HEAD(task_queue, task);

typedef struct task {
  TAILQ_ENTRY(task) t_link;
  const char *t_name;
  timer_t t_timer;
  uint8_t t_basepri;
  void *t_psp;
  uint8_t t_stack[0];
} task_t;

extern task_t *curtask;


task_t *task_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
                    const char *name);

