#pragma once

#include <stdarg.h>

#define CPU_STACK_ALIGNMENT 16

#define MIN_STACK_SIZE 1024

void *cpu_stack_init(uint64_t *stack, void *entry,
                     void (*thread_exit)(void *), int nargs, va_list ap);

typedef struct cpu {
  sched_cpu_t sched;
} cpu_t;

static inline void
cpu_fpu_enable(int on)
{
}

static inline void
cpu_stack_redzone(thread_t *t)
{
}

void cpu_fpu_ctx_init(int *ctx);
