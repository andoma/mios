#include "irq.h"

#include "reg.h"

#include <stdint.h>
#include <stdio.h>
#include <mios/mios.h>


#define GIC_BASE 0x8000000

#define GIC_GICD_BASE GIC_BASE

#define GICD_CTLR          (GIC_GICD_BASE + 0x000)
#define GICD_TYPER         (GIC_GICD_BASE + 0x004)
#define GICD_IIDR          (GIC_GICD_BASE + 0x008)
#define GICD_IGROUPR(x)    (GIC_GICD_BASE + 0x080 + (x) * 4)
#define GICD_ISENABLER(x)  (GIC_GICD_BASE + 0x100 + (x) * 4)
#define GICD_ICENABLER(x)  (GIC_GICD_BASE + 0x180 + (x) * 4)
#define GICD_ISPENDR(x)    (GIC_GICD_BASE + 0x200 + (x) * 4)
#define GICD_ICPENDR(x)    (GIC_GICD_BASE + 0x280 + (x) * 4)
#define GICD_IPRIORITYR(x) (GIC_GICD_BASE + 0x400 + (x))
#define GICD_ITARGETSR(x)  (GIC_GICD_BASE + 0x800 + (x))
#define GICD_ICFGR(x)      (GIC_GICD_BASE + 0xc00 + (x) * 4)
#define GICD_SGIR          (GIC_GICD_BASE + 0xf00)



#define GIC_GICR_BASE (GIC_BASE + 0xa0000)


#define GICR_SGI_OFFSET  0x10000
#define GICR_VLPI_OFFSET 0x20000

#define GICR_WAKER         (GIC_GICR_BASE + 0x014)

#define GICR_IGROUPR(x)    (GIC_GICR_BASE + GICR_SGI_OFFSET + 0x080 + (x) * 4)
#define GICR_ISENABLER(x)  (GIC_GICR_BASE + GICR_SGI_OFFSET + 0x100 + (x) * 4)
#define GICR_ICENABLER(x)  (GIC_GICR_BASE + GICR_SGI_OFFSET + 0x180 + (x) * 4)
#define GICR_ISPENDR(x)    (GIC_GICR_BASE + GICR_SGI_OFFSET + 0x200 + (x) * 4)
#define GICR_ICPENDR(x)    (GIC_GICR_BASE + GICR_SGI_OFFSET + 0x280 + (x) * 4)
#define GICR_IPRIORITYR(x) (GIC_GICR_BASE + GICR_SGI_OFFSET + 0x400 + (x))


typedef struct irq_handler {
  void (*fn)(void *arg);
  void *arg;
} irq_handler_t;


static void
sgi_enable(int sgi, int level)
{
  uint32_t reg = sgi >> 5;
  uint32_t bit = sgi & 0x1f;

  uint32_t grp = reg_rd(GICR_IGROUPR(reg));
  grp |= 1 << bit;
  reg_wr(GICR_IGROUPR(reg), grp);
  reg_wr(GICR_ICPENDR(reg), (1 << bit));
  reg_wr(GICR_ISENABLER(reg), (1 << bit));
  reg_wr8(GICR_IPRIORITYR(sgi), IRQ_LEVEL_TO_PRI(level));
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

  uint32_t grp = reg_rd(GICD_IGROUPR(reg));
  grp |= 1 << bit;
  reg_wr(GICD_IGROUPR(reg), grp);
  reg_wr(GICD_ICPENDR(reg), (1 << bit));
  reg_wr(GICD_ISENABLER(reg), (1 << bit));
  reg_wr8(GICD_IPRIORITYR(irq), IRQ_LEVEL_TO_PRI(level));
}

static irq_handler_t irqs[128];

void
trap_irq(uint32_t irq)
{
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

void irq_disable(int irq);



static void  __attribute__((constructor(102)))
irq_init(void)
{
  reg_wr(GICD_CTLR, 7);
#if 0
  printf("GICD_CTRL:%x\n", reg_rd(GIC_BASE + 0x0));
  printf("GICD_IIDR:%x\n", reg_rd(GIC_BASE + 0x8));
  printf("GICD_CTRL:%x\n", reg_rd(GIC_BASE + 0x0));
#endif

  reg_wr(GICR_WAKER, 0);

  asm volatile ("msr icc_sre_el1, %0\n\t" : : "r" (1));
  asm volatile ("msr icc_pmr_el1, %0\n\t" : : "r" (0xff));
  asm volatile ("msr icc_igrpen1_el1, %0\n\t" : : "r" (1));

  sgi_enable(0, IRQ_LEVEL_SWITCH);
}
