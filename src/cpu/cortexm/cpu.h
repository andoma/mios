#pragma once

#include <stdint.h>

#include "task.h"

#define FPU_CTX_SIZE (33 * 4) // s0 ... s31 + FPSCR

#define CPU_STACK_ALIGNMENT 32
#define CPU_STACK_REDZONE_SIZE 32

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

static inline void
cpu_fpu_enable(int on)
{
  static volatile unsigned int * const CPACR = (unsigned int *)0xe000ed88;
  *CPACR = on ? 0xf << 20 : 0;
}


static inline void
cpu_stack_redzone(task_t *t)
{
  static volatile unsigned int * const MPU_RBAR = (unsigned int *)0xe000ed9c;
  *MPU_RBAR = (intptr_t)t->t_sp_bottom | 0x17;
}

void cpu_fpu_ctx_init(int *ctx);
