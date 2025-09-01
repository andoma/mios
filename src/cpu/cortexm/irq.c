#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <mios/mios.h>

#include "irq.h"
#include "mpu.h"
#include "cache.h"

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

#define VECTOR_COUNT (16 + CORTEXM_IRQ_COUNT)
#define VECTOR_ALIGN (VECTOR_COUNT >= 128 ? 0x400 : 0x200)

void
irq_enable_fn(int irq, int level, void (*fn)(void))
{
  assert(irq < CORTEXM_IRQ_COUNT);
  int q = irq_forbid(IRQ_LEVEL_ALL);

  mpu_protect_code(0);

  uint32_t curvecs = *VTOR;

  if(curvecs == (uint32_t)&vectors) {
    // Not yet relocated
    const size_t vecsize = VECTOR_COUNT * sizeof(void *);
    void *p = xalloc(vecsize, VECTOR_ALIGN, MEM_TYPE_VECTOR_TABLE);
    memcpy(p, &vectors, vecsize);
    *VTOR = (uint32_t)p;
  }

  uint32_t *vtable = (uint32_t *)*VTOR;
  vtable[irq + 16] = (uint32_t)fn;
#ifdef HAVE_BASEPRI
  NVIC_IPR[irq] = IRQ_LEVEL_TO_PRI(level);
#endif
  NVIC_ISER[(irq >> 5) & 7] |= 1 << (irq & 0x1f);

  dcache_op(vtable, VECTOR_COUNT * sizeof(void *), DCACHE_CLEAN);
  icache_invalidate();
  mpu_protect_code(1);
  irq_permit(q);
}


/*
        push    {r4, r5, r6, lr}
        mov.w   r4, #0xe000e000
        mov.w   r3, #0xf00000
        ldr.w   r5, [r4, #0xd88]
        str.w   r3, [r4, #0xd88]
        isb     sy
        vmrs    r6, fpscr
        vpush   {s0-s15}
        ldr     r3, [pc, #20]
        ldr     r0, [pc, #20]
        blx     r3
        vpop    {s0-s15}
        vmsr    fpscr, r6
        str.w   r5, [r4, #0xd88]
        pop     {r4, r5, r6, pc}
 */
static const uint8_t irq_fpu_trampoline[] = {
  0x70, 0xb5, 0x4f, 0xf0, 0xe0, 0x24, 0x4f, 0xf4,
  0x70, 0x03, 0xd4, 0xf8, 0x88, 0x5d, 0xc4, 0xf8,
  0x88, 0x3d, 0xbf, 0xf3, 0x6f, 0x8f, 0xf1, 0xee,
  0x10, 0x6a, 0x2d, 0xed, 0x10, 0x0a, 0x05, 0x4b,
  0x05, 0x48, 0x98, 0x47, 0xbd, 0xec, 0x10, 0x0a,
  0xe1, 0xee, 0x10, 0x6a, 0xc4, 0xf8, 0x88, 0x5d,
  0x70, 0xbd
};

void
irq_enable_fn_fpu(int irq, int level, void (*fn)(void *arg), void *arg)
{
  mpu_protect_code(0);
  uint32_t *p = xalloc(60, 0, MEM_TYPE_CODE);
  memcpy(p, irq_fpu_trampoline, sizeof(irq_fpu_trampoline));
  p[13] = (uint32_t)fn;
  p[14] = (uint32_t)arg;

  dcache_op(p, sizeof(irq_fpu_trampoline), DCACHE_CLEAN);

  irq_enable_fn(irq, level, (void *)p + 1);
  mpu_protect_code(1);
}

void
irq_enable_fn_arg(int irq, int level, void (*fn)(void *arg), void *arg)
{
  mpu_protect_code(0);
  uint32_t *p = xalloc(16, 0, MEM_TYPE_CODE);

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

  dcache_op(p, 16, DCACHE_CLEAN);

  irq_enable_fn(irq, level, (void *)p + 1);
  mpu_protect_code(1);
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

extern void cpu_softreset(uint32_t vtor) __attribute__((noreturn));

void
softreset(uint32_t vtor)
{
  asm volatile ("cpsid i;isb;dsb");

  *VTOR = vtor;

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
  cpu_softreset(vtor);
}
