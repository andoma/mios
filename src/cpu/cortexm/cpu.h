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
                     void (*thread_exit)(void *));

typedef struct cpu {
  sched_cpu_t sched;
} cpu_t;



static inline void
cpu_fpu_enable(int on)
{
  static volatile unsigned int * const CPACR = (unsigned int *)0xe000ed88;
  *CPACR = on ? 0xf << 20 : 0;
}

static inline void
cpu_unmap_null(int size_power_of_two)
{
  static volatile uint32_t * const MPU_RBAR = (uint32_t *)0xe000ed9c;
  static volatile uint32_t * const MPU_RASR = (uint32_t *)0xe000eda0;

  size_power_of_two--;
  *MPU_RBAR = 0 | 0x16;                     // Set MPU to region 6 and address 0

  // Map no access and no-execute
  *MPU_RASR = (1 << 28) | (size_power_of_two << 1) | 1;
}


static inline void
cpu_stack_redzone(thread_t *t)
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
