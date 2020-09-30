#include <string.h>

#include "cpu.h"

cpu_t cpu0 = {
  .name = "cpu0",
};

static volatile unsigned int * const ACTLR = (unsigned int *)0xe000e008;

static volatile unsigned int * const SHCSR = (unsigned int *)0xe000ed24;

static volatile unsigned int * const FPCCR = (unsigned int *)0xe000ef34;

static volatile unsigned int * const MPU_CTRL = (unsigned int *)0xe000ed94;
static volatile unsigned int * const MPU_RBAR = (unsigned int *)0xe000ed9c;
static volatile unsigned int * const MPU_RASR = (unsigned int *)0xe000eda0;

#ifdef TASK_ACCOUNTING
volatile unsigned int *DWT_CONTROL  = (volatile unsigned int *)0xE0001000;
volatile unsigned int *DWT_LAR      = (volatile unsigned int *)0xE0001FB0;
volatile unsigned int *SCB_DEMCR    = (volatile unsigned int *)0xE000EDFC;
#endif

static void __attribute__((constructor(150)))
cpu_init(void)
{
  if(0) {
    // Enable this to disable instruction folding, write buffer and
    // interrupt of multi-cycle instructions
    // This can help generate more precise busfauls
    *ACTLR |= 7;
  }
  extern void *idle_stack;
  task_init_cpu(&cpu0.sched, cpu0.name, &idle_stack);

  *FPCCR = 0; // No FPU lazy switching, we deal with it ourselves

  *SHCSR |= 0x7 << 16; // Enable UsageFault, BusFault, MemFault handlers

  // MPU region 7 is used as 32 byte stack redzone

  *MPU_RBAR = (intptr_t)&idle_stack | 0x17; // Set MPU to region 7
  *MPU_RASR = (4 << 1) | 1; // 2^(4 + 1) = 32 byte + enable
  *MPU_CTRL = 5; // Enable MPU

#ifdef TASK_ACCOUNTING
  // Enable cycle counter
  *SCB_DEMCR |= 0x01000000;
  *DWT_LAR = 0xC5ACCE55; // unlock
  *DWT_CONTROL = 1;
#endif
}


void *
cpu_stack_init(uint32_t *stack, void *(*entry)(void *arg), void *arg,
               void (*thread_exit)(void))
{
  stack = (uint32_t *)(((intptr_t)stack) & ~7);
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
