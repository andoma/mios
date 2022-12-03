#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <mios/mios.h>
#include "irq.h"

static volatile unsigned int * const ICSR    = (unsigned int *)0xe000ed04;

void
irq(void)
{
  panic("Unexpected IRQ %d", *ICSR & 0xff);
}

#include "irq_alias.h"


static volatile unsigned int * const NVIC_ISER = (unsigned int *)0xe000e100;
static volatile unsigned int * const NVIC_ICER = (unsigned int *)0xe000e180;
static volatile unsigned int * const NVIC_ICPR = (unsigned int *)0xe000e280;
static volatile unsigned int * const VTOR  = (unsigned int *)0xe000ed08;

#ifdef HAVE_BASEPRI
static volatile uint8_t * const NVIC_IPR  = (uint8_t *)0xe000e400;
static volatile unsigned int * const SYST_SHPR3 = (unsigned int *)0xe000ed20;
#endif

extern uint32_t vectors[];

void
irq_enable(int irq, int level)
{
#ifdef HAVE_BASEPRI
  NVIC_IPR[irq] = IRQ_LEVEL_TO_PRI(level);
#endif
  NVIC_ISER[(irq >> 5) & 7] |= 1 << (irq & 0x1f);
}

void
irq_disable(int irq)
{
  NVIC_ICER[(irq >> 5) & 7] |= 1 << (irq & 0x1f);
}


void
irq_enable_fn(int irq, int level, void (*fn)(void))
{
  assert(irq < CORTEXM_IRQ_COUNT);
  int q = irq_forbid(IRQ_LEVEL_ALL);

  uint32_t curvecs = *VTOR;

  if(curvecs == (uint32_t)&vectors) {
    // Not yet relocated
    const size_t vecsize = (16 + CORTEXM_IRQ_COUNT) * sizeof(void *);
    void *p = xalloc(vecsize, 0x200, 0);
    memcpy(p, &vectors, vecsize);
    *VTOR = (uint32_t)p;
  }

  uint32_t *vtable = (uint32_t *)*VTOR;
  vtable[irq + 16] = (uint32_t)fn;
#ifdef HAVE_BASEPRI
  NVIC_IPR[irq] = IRQ_LEVEL_TO_PRI(level);
#endif
  NVIC_ISER[(irq >> 5) & 7] |= 1 << (irq & 0x1f);
  irq_permit(q);
}

void
irq_enable_fn_arg(int irq, int level, void (*fn)(void *arg), void *arg)
{
  uint32_t *p = malloc(16);

  /*
    ldr r3, [pc, #4]  // fn -> r3
    nop
    ldr r0, [pc, #4]  // arg -> r0
    bx r3
  */

  // NOP is encoded differently on thumb and thumb2
#ifdef __thumb2__
  p[0] = 0xbf004b01;
#else
  p[0] = 0x46c04b01;
#endif
  p[1] = 0x47184801;
  p[2] = (uint32_t)fn;
  p[3] = (uint32_t)arg;
  irq_enable_fn(irq, level, (void *)p + 1);
}

static void __attribute__((constructor(101)))
irq_init(void)
{
  *VTOR = (uint32_t)&vectors;
#ifdef HAVE_BASEPRI
  *SYST_SHPR3 =
    (IRQ_LEVEL_TO_PRI(IRQ_LEVEL_CLOCK) << 24) |
    (IRQ_LEVEL_TO_PRI(IRQ_LEVEL_SWITCH) << 16);
#endif
}

extern void cpu_softreset(void) __attribute__((noreturn));

void
softreset(void)
{
  asm volatile ("cpsid i;isb;dsb");

  *VTOR = 0;

  for(int i = 0; i < (CORTEXM_IRQ_COUNT + 31) / 32; i++) {
    NVIC_ICER[i] = 0xffffffff;
    NVIC_ICPR[i] = 0;
  }
#ifdef HAVE_BASEPRI
  for(int irq = 0; irq < CORTEXM_IRQ_COUNT; irq++) {
    NVIC_IPR[irq] = 0;
  }

  *SYST_SHPR3 = 0;
#endif
  cpu_softreset();
}
