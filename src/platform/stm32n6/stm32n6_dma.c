#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/task.h>

#include "irq.h"
#include "stm32n6_clk.h"
#include "stm32n6_dma.h"

// HPDMA1: 16 channels, instances 0-15
// AHB5 bus, nonsecure alias (Table 3 in RM0486)
#define HPDMA1_BASE 0x48020000

#define DMA_NUM_CHANNELS 16

static const int dma_irq_base = 68;  // HPDMA1 ch0 = IRQ 68

#include "platform/stm32/stm32_dma_v3.c"

stm32_dma_instance_t
stm32_dma_alloc(uint32_t resource_id, const char *name)
{
  return stm32_dma_alloc_instance_v3(HPDMA1_BASE, CLK_HPDMA1,
                                     0, 16, 68,
                                     resource_id & 0xff, name);
}

// HPDMA1 channel IRQ handlers (IRQ 68-83)
void irq_68(void) { dma_v3_irq(0); }
void irq_69(void) { dma_v3_irq(1); }
void irq_70(void) { dma_v3_irq(2); }
void irq_71(void) { dma_v3_irq(3); }
void irq_72(void) { dma_v3_irq(4); }
void irq_73(void) { dma_v3_irq(5); }
void irq_74(void) { dma_v3_irq(6); }
void irq_75(void) { dma_v3_irq(7); }
void irq_76(void) { dma_v3_irq(8); }
void irq_77(void) { dma_v3_irq(9); }
void irq_78(void) { dma_v3_irq(10); }
void irq_79(void) { dma_v3_irq(11); }
void irq_80(void) { dma_v3_irq(12); }
void irq_81(void) { dma_v3_irq(13); }
void irq_82(void) { dma_v3_irq(14); }
void irq_83(void) { dma_v3_irq(15); }
