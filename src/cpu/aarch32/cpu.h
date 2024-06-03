#pragma once

#include <mios/task.h>

#define MIN_STACK_SIZE 1024

#define CPU_STACK_ALIGNMENT 8


typedef struct cpu {
  sched_cpu_t sched;
} cpu_t;

extern struct cpu cpu0;

static inline struct cpu *curcpu(void) { return &cpu0; }

static inline void
cpu_stack_redzone(thread_t *t)
{
}


void *
cpu_stack_init(uint32_t *stack, void *(*entry)(void *arg), void *arg,
               void (*thread_exit)(void *));


static inline uint32_t
cpu_get_periphbase(void)
{
  uint32_t result;
  asm volatile ("mrc p15, #4, %0, c15, c0, #0" : "=r" (result));
  return result;
}

#if 0
static inline uint32_t
get_cpsr(void)
{
  uint32_t cpsr;
  asm volatile("mrs %0, CPSR" : "=r"(cpsr));
  return cpsr;
}


static inline uint32_t
get_spsr(void)
{
  uint32_t spsr;
  asm volatile("mrs %0, SPSR" : "=r"(spsr));
  return spsr;
}
#endif
