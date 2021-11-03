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

#include "platform/stm32/stm32_dma_v2.c"


stm32_dma_instance_t
stm32_dma_alloc(uint32_t resource_id,
                void (*cb)(stm32_dma_instance_t,
                           void *arg, error_t err),
                void *arg, const char *name,
                int irq_level)
{
  clk_enable(CLK_DMAMUX1);
  stm32_dma_instance_t instance =
    stm32_dma_alloc_instance(-1, cb, arg, name, irq_level);
  reg_wr(DMAMUX1_CxCR(instance), resource_id);
  return instance;
}
