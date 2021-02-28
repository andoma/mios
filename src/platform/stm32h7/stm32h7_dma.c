#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/task.h>

#include "irq.h"
#include "stm32h7.h"
#include "stm32h7_dma.h"
#include "stm32h7_clk.h"

#define DMAMUX1_BASE 0x40020800
#define DMAMUX1_CxCR(x) (DMAMUX1_BASE + 4 * (x))

#define DMA_BASE(x) (0x40020000 + (x) * 0x400)

#include "platform/stm32/stm32_dma.c"


stm32_dma_instance_t
stm32h7_dma_alloc(int resource_id,
                  void (*cb)(stm32_dma_instance_t instance,
                             void *arg, error_t err),
                  void *arg, const char *name)
{
  stm32_dma_instance_t instance = stm32_dma_alloc(-1, cb, arg, name);
  reg_wr(DMAMUX1_CxCR(instance), resource_id);
  return instance;
}
