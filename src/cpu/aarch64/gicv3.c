#include "irq.h"
#include "gicv3.h"

#include "reg.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <mios/mios.h>

#define GICD_CTLR          (0x000)
#define GICD_TYPER         (0x004)
#define GICD_IIDR          (0x008)
#define GICD_IGROUPR(x)    (0x080 + (x) * 4)
#define GICD_ISENABLER(x)  (0x100 + (x) * 4)
#define GICD_ICENABLER(x)  (0x180 + (x) * 4)
#define GICD_ISPENDR(x)    (0x200 + (x) * 4)
#define GICD_ICPENDR(x)    (0x280 + (x) * 4)
#define GICD_IPRIORITYR(x) (0x400 + (x))
#define GICD_ITARGETSR(x)  (0x800 + (x))
#define GICD_ICFGR(x)      (0xc00 + (x) * 4)
#define GICD_SGIR          (0xf00)



#define GICR_SGI_OFFSET  0x10000

#define GICR_IIDR          (0x004)
#define GICR_WAKER         (0x014)

#define GICR_IGROUPR(x)    (GICR_SGI_OFFSET + 0x080 + (x) * 4)
#define GICR_ISENABLER(x)  (GICR_SGI_OFFSET + 0x100 + (x) * 4)
#define GICR_ICENABLER(x)  (GICR_SGI_OFFSET + 0x180 + (x) * 4)
#define GICR_ISPENDR(x)    (GICR_SGI_OFFSET + 0x200 + (x) * 4)
#define GICR_ICPENDR(x)    (GICR_SGI_OFFSET + 0x280 + (x) * 4)
#define GICR_IPRIORITYR(x) (GICR_SGI_OFFSET + 0x400 + (x))


typedef struct irq_handler {
  void (*fn)(void *arg);
  void *arg;
} irq_handler_t;


static void
sgi_enable(int sgi, int level)
{
  uint32_t reg = sgi >> 5;
  uint32_t bit = sgi & 0x1f;

  long base = gicr_base();

  uint32_t grp = reg_rd(base + GICR_IGROUPR(reg));
  grp |= 1 << bit;
  reg_wr(base + GICR_IGROUPR(reg), grp);
  reg_wr(base + GICR_ICPENDR(reg), (1 << bit));
  reg_wr(base + GICR_ISENABLER(reg), (1 << bit));
  reg_wr8(base + GICR_IPRIORITYR(sgi), IRQ_LEVEL_TO_PRI(level));
}




void
irq_enable(int irq, int level)
{
  if(irq < 32) {
    sgi_enable(irq, level);
    return;
  }

  uint32_t reg = irq >> 5;
  uint32_t bit = irq & 0x1f;

  uint32_t grp = reg_rd(GIC_GICD_BASE + GICD_IGROUPR(reg));
  grp |= 1 << bit;
  reg_wr(GIC_GICD_BASE + GICD_IGROUPR(reg), grp);
  reg_wr(GIC_GICD_BASE + GICD_ICPENDR(reg), (1 << bit));
  reg_wr(GIC_GICD_BASE + GICD_ISENABLER(reg), (1 << bit));
  reg_wr8(GIC_GICD_BASE + GICD_IPRIORITYR(irq), IRQ_LEVEL_TO_PRI(level));
}

static irq_handler_t irqs[1024];

void
trap_irq(uint32_t irq)
{
  if(irq == 1023)
    return;

  if(irqs[irq].fn == NULL)
    panic("Spurious IRQ %d", irq);
  irqs[irq].fn(irqs[irq].arg);
}


void
irq_enable_fn_arg(int irq, int level, void (*fn)(void *arg), void *arg)
{
  irqs[irq].fn = fn;
  irqs[irq].arg = arg;
  irq_enable(irq, level);
}


void
irq_enable_fn(int irq, int level, void (*fn)(void))
{
  irq_enable_fn_arg(irq, level, (void *)fn, NULL);
}

static void
sgi_disable(int sgi)
{
  panic("sgi_disable");
}


void
irq_disable(int irq)
{
  if(irq < 32) {
    sgi_disable(irq);
    return;
  }

  uint32_t reg = irq >> 5;
  uint32_t bit = irq & 0x1f;

  int q = irq_forbid(IRQ_LEVEL_ALL);

  uint32_t grp = reg_rd(GIC_GICD_BASE + GICD_IGROUPR(reg));
  grp &= ~(1 << bit);
  reg_wr(GIC_GICD_BASE + GICD_IGROUPR(reg), grp);
  reg_wr(GIC_GICD_BASE + GICD_ICPENDR(reg), (1 << bit));
  reg_wr(GIC_GICD_BASE + GICD_ICENABLER(reg), (1 << bit));
  irq_permit(q);
}



static void  __attribute__((constructor(105)))
irq_init(void)
{
  reg_wr(GIC_GICD_BASE + GICD_CTLR, 7);

  long base = gicr_base();
  printf("GICv3: GICR @ 0x%lx\n", base);

  reg_wr(base + GICR_WAKER, 0);

  asm volatile ("msr icc_sre_el1, %0\n\t" : : "r" (1));
  asm volatile ("msr icc_pmr_el1, %0\n\t" : : "r" (0xff));
  asm volatile ("msr icc_igrpen1_el1, %0\n\t" : : "r" (1));

  sgi_enable(0, IRQ_LEVEL_SWITCH);
}
