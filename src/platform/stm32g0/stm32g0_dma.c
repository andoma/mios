#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/task.h>

#include "irq.h"
#include "stm32g0.h"
#include "platform/stm32/stm32_dma.h"
#include "stm32g0_clk.h"

#define DMAMUX1_BASE 0x40020800
#define DMAMUX1_CxCR(x) (DMAMUX1_BASE + 4 * (x))

#define DMA_BASE(x) (0x40020000 + (x) * 0x400)


static const uint8_t dma_irqmap[16] = { 9,10,10,11,11,11,11,11};

#include "platform/stm32/stm32_dma_v2.c"

stm32_dma_instance_t
stm32_dma_alloc(uint32_t resource_id, const char *name)
{
  // NB: CLK_DMAMUX is not available on stm32g0, rather it's piggybacking
  // on any other enabled DMA instance
  stm32_dma_instance_t instance = stm32_dma_alloc_instance(-1, name);
  reg_wr(DMAMUX1_CxCR(instance), resource_id);
  return instance;
}


void irq_9(void)
{
  dma_irq(0);
}

void irq_10(void)
{
  const uint32_t isr = reg_rd(DMA_ISR(0));
  if(isr & 0x10)
    dma_irq(1);
  if(isr & 0x100)
    dma_irq(2);
}
