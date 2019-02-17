#pragma once

#include <stdint.h>

#include "task.h"

#define MIN_STACK_SIZE 256

void *cpu_stack_init(uint32_t *stack, void *(*entry)(void *arg), void *arg,
                     void (*thread_exit)(void));

typedef struct cpu {
  sched_cpu_t sched;
  char name[8];
} cpu_t;

extern cpu_t cpu0;

static inline cpu_t *
curcpu(void)
{
  return &cpu0;
}

void cpu_init(void);
