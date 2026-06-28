#include "cpu.h"

#include <mios/mios.h>

static volatile uint32_t * const SHCSR    = (uint32_t *)0xe000ed24;
static volatile uint32_t * const FPCCR    = (uint32_t *)0xe000ef34;

static void __attribute__((constructor(150)))
cortexm33_init(void)
{
  *FPCCR = 0; // No FPU lazy switching, we deal with it ourselves
  *SHCSR |= 0x7 << 16; // Enable UsageFault, BusFault, MemFault handlers
}


static void __attribute__((destructor(150)))
cortexm33_fini(void)
{
  *SHCSR &= ~(0x7 << 16); // Disable UsageFault, BusFault, MemFault handlers
}
