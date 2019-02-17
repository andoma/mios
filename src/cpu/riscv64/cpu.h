#pragma once

#include <stdint.h>
#include "task.h"

#define MIN_STACK_SIZE 4096

void *cpu_stack_init(uint64_t *stack, void *(*entry)(void *arg), void *arg,
                     void (*thread_exit)(void));

typedef struct cpu {
  sched_cpu_t sched;
  char name[8];
} cpu_t;

void cpu_init(void);

static inline cpu_t *
curcpu(void)
{
  cpu_t *c;
  asm volatile ("csrr %0, mscratch\n\t" : "=r" (c));
  return c;
}
