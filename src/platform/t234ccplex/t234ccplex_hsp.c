#include "t234_hsp.h"

#include "reg.h"

#include "t234_hsp.c"

static void  __attribute__((constructor(105)))
tegra234_hsp_init(void)
{
  g_aon_hsp.irq_route = 1;
  irq_enable_fn_arg(133 + 32, IRQ_LEVEL_IO, hsp_aon_irq, &g_aon_hsp);

  g_top0_hsp.irq_route = 0;
  irq_enable_fn_arg(120 + 32, IRQ_LEVEL_IO, hsp_top0_irq, &g_top0_hsp);

  g_top1_hsp.irq_route = 0;
  irq_enable_fn_arg(128 + 32, IRQ_LEVEL_NET, hsp_top1_irq, &g_top1_hsp);
}
