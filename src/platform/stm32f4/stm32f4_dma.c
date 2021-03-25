#include <stdlib.h>
#include <mios/task.h>
#include <mios/error.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include "irq.h"

#include "stm32f4_dma.h"
#include "stm32f4_clk.h"

#define DMA_BASE(x) (0x40026000 + (x) * 0x400)

#include "platform/stm32/stm32_dma.c"

stm32_dma_instance_t
stm32f4_dma_alloc_fixed(int controller, int stream, int channel,
                        void (*cb)(stm32_dma_instance_t instance,
                                   void *arg, error_t err),
                        void *arg, const char *name)
{
  assert(channel == 0);
  return stm32_dma_alloc(1 << (controller * 8 + stream), cb, arg, name);
}
