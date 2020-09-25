#include <string.h>

#include "cpu.h"

cpu_t cpu0 = {
  .name = "cpu0",
};

static volatile unsigned int * const SHCSR = (unsigned int *)0xe000ed24;

static volatile unsigned int * const FPCCR = (unsigned int *)0xe000ef34;

static volatile unsigned int * const MPU_CTRL = (unsigned int *)0xe000ed94;
static volatile unsigned int * const MPU_RBAR = (unsigned int *)0xe000ed9c;
static volatile unsigned int * const MPU_RASR = (unsigned int *)0xe000eda0;



static void __attribute__((constructor(150)))
cpu_init(void)
{
  extern void *idle_stack;
  task_init_cpu(&cpu0.sched, cpu0.name, &idle_stack);

  *FPCCR = 0; // No FPU lazy switching, we deal with it ourselves

  *SHCSR |= 0x7 << 16; // Enable UsageFault, BusFault, MemFault handlers

  // MPU region 7 is used as 32 byte stack redzone

  *MPU_RBAR = (intptr_t)&idle_stack | 0x17; // Set MPU to region 7
  *MPU_RASR = (4 << 1) | 1; // 2^(4 + 1) = 32 byte + enable
  *MPU_CTRL = 5; // Enable MPU
}


void *
cpu_stack_init(uint32_t *stack, void *(*entry)(void *arg), void *arg,
               void (*thread_exit)(void))
{
  *--stack = 0x21000000;  // PSR
  *--stack = (uint32_t) entry;
  *--stack = (uint32_t) thread_exit;
  for(int i = 0; i < 13; i++)
    *--stack = 0;
  stack[8] = (uint32_t) arg; // r0
  return stack;
}

void
cpu_fpu_ctx_init(int *ctx)
{
  memset(ctx, 0, sizeof(int) * 32);
  ctx[32] = 1 << 24; // Enable flush-to-zero
}
