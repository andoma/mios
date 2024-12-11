#include "cpu.h"

static volatile uint32_t * const SHCSR    = (uint32_t *)0xe000ed24;
static volatile uint32_t * const FPCCR    = (uint32_t *)0xe000ef34;

static volatile uint32_t * const MPU_CTRL = (uint32_t *)0xe000ed94;
static volatile uint32_t * const MPU_RBAR = (uint32_t *)0xe000ed9c;
static volatile uint32_t * const MPU_RASR = (uint32_t *)0xe000eda0;

static void __attribute__((constructor(151)))
cortexm4f_init(void)
{
  *FPCCR = 0; // No FPU lazy switching, we deal with it ourselves

  *SHCSR |= 0x7 << 16; // Enable UsageFault, BusFault, MemFault handlers

  if(CPU_STACK_REDZONE_SIZE == 32) {
    // MPU region 7 is used as 32 byte stack redzone

    void *idle_stack = ((thread_t *)(curcpu()->sched.idle))->t_sp_bottom;
    *MPU_RBAR = (intptr_t)idle_stack | 0x17; // Set MPU to region 7
    *MPU_RASR = (4 << 1) | 1; // 2^(4 + 1) = 32 byte + enable
  }

  *MPU_CTRL = 5; // Enable MPU
}
