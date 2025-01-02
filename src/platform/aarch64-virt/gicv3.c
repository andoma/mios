#include "irq.h"

#include "reg.h"

#include <stdint.h>



#define GIC_BASE 0x8000000

#define GIC_GICD_BASE GIC_BASE
#define GIC_GICC_BASE (GIC_BASE + 0x10000)


#define GICC_CTLR     (GIC_GICC_BASE + 0x000)
#define GICC_PMR      (GIC_GICC_BASE + 0x004)
#define GICC_BPR      (GIC_GICC_BASE + 0x008)
#define GICC_IAR      (GIC_GICC_BASE + 0x00C)
#define GICC_EOIR     (GIC_GICC_BASE + 0x010)
#define GICC_RPR      (GIC_GICC_BASE + 0x014)
#define GICC_HPIR     (GIC_GICC_BASE + 0x018)
#define GICC_ABPR     (GIC_GICC_BASE + 0x01C)
#define GICC_IIDR     (GIC_GICC_BASE + 0x0FC)

#define GICD_CTLR          (GIC_GICD_BASE + 0x000)
#define GICD_TYPER         (GIC_GICD_BASE + 0x004)
#define GICD_IIDR          (GIC_GICD_BASE + 0x008)
#define GICD_ISENABLER(x)  (GIC_GICD_BASE + 0x100 + (x) * 4)
#define GICD_ICENABLER(x)  (GIC_GICD_BASE + 0x180 + (x) * 4)
#define GICD_ISPENDR(x)    (GIC_GICD_BASE + 0x200 + (x) * 4)
#define GICD_ICPENDR(x)    (GIC_GICD_BASE + 0x280 + (x) * 4)
#define GICD_IPRIORITYR(x) (GIC_GICD_BASE + 0x400 + (x))
#define GICD_ITARGETSR(x)  (GIC_GICD_BASE + 0x800 + (x))
#define GICD_ICFGR(x)      (GIC_GICD_BASE + 0xc00 + (x) * 4)
#define GICD_SGIR          (GIC_GICD_BASE + 0xf00)


static void
irq_enable(int irq, int level)
{
  uint32_t reg = irq >> 5;
  uint32_t bit = irq & 0x1f;

  reg_wr(GICD_ICPENDR(reg), (1 << bit));
  reg_wr(GICD_ISENABLER(reg), (1 << bit));
  reg_wr8(GICD_IPRIORITYR(irq), IRQ_LEVEL_TO_PRI(level));
}


static void  __attribute__((constructor(102)))
irq_init(void)
{
  reg_wr(GICC_PMR, 0xf8);

  reg_wr(GICD_CTLR, 1);
  reg_wr(GICC_CTLR, 1);

}
