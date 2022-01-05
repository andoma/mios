#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/task.h>

#include "irq.h"
#include "stm32wb.h"
#include "platform/stm32/stm32_dma.h"
#include "stm32wb_clk.h"

#define DMAMUX1_BASE 0x40020800
#define DMAMUX1_CxCR(x) (DMAMUX1_BASE + 4 * (x))

#define DMA_BASE(x) (0x40020000 + (x) * 0x400)

static const uint8_t dma_irqmap[16] = {
  11,12,13,14,15,16,17,96,56,57,58,59,60,97,98,99
};

#include "platform/stm32/stm32_dma_v2.c"


stm32_dma_instance_t
stm32_dma_alloc(uint32_t resource_id, const char *name)
{
  clk_enable(CLK_DMAMUX1);
  stm32_dma_instance_t instance = stm32_dma_alloc_instance(-1, name);
  reg_wr(DMAMUX1_CxCR(instance), resource_id);
  return instance;
}



void irq_11(void) { dma_irq(0); }
void irq_12(void) { dma_irq(1); }
void irq_13(void) { dma_irq(2); }
void irq_14(void) { dma_irq(3); }
void irq_15(void) { dma_irq(4); }
void irq_16(void) { dma_irq(5); }
void irq_17(void) { dma_irq(6); }
void irq_96(void) { dma_irq(7); }
void irq_56(void) { dma_irq(8); }
void irq_57(void) { dma_irq(9); }
void irq_58(void) { dma_irq(10); }
void irq_59(void) { dma_irq(11); }
void irq_60(void) { dma_irq(12); }
void irq_97(void) { dma_irq(13); }
void irq_98(void) { dma_irq(14); }
void irq_99(void) { dma_irq(15); }

