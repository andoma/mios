#pragma once

#include <stdint.h>

#include <mios/task.h>

#ifdef CPU_STACK_REDZONE_SIZE
#define CPU_STACK_ALIGNMENT CPU_STACK_REDZONE_SIZE
#else
#define CPU_STACK_ALIGNMENT 8
#endif

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



static inline uint32_t
cpu_cycle_counter(void)
{
  volatile unsigned int *DWT_CYCCNT   = (volatile unsigned int *)0xE0001004;
  return *DWT_CYCCNT;
}



// Return 1 if lock was acquired
static inline int
cpu_mutex_lock_fast(mutex_t *m, task_t *curtask)
{
  // (bit 2 indicate support for 32bit LDREX/STREX)
#if defined(__ARM_FEATURE_LDREX) && __ARM_FEATURE_LDREX & 4
  task_t *cur;
  int status;
  asm volatile("ldrex %0, [%2]\n"
               "mov %1, #1\n"
               "teq %0, #0\n"
               "it eq\n"
               "strexeq %1, %3, [%2]\n"
               : "=&r"(cur), "=&r"(status)
               : "r"(m), "r"(curtask)
               : "cc", "memory");
  return !status;
#else
  return 0; // Force slow path
#endif
}
