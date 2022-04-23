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

#include "platform/stm32/stm32_dma_v1.c"

stm32_dma_instance_t
stm32_dma_alloc(uint32_t resource_id, const char *name)
{
  uint32_t sa = (resource_id >> 16) & 0xff;
  uint32_t sb = resource_id & 0xff;

  uint32_t mask = (1 << sb);
  if(sa != 0xff) {
    mask |= (1 << sa);
  }

  stm32_dma_instance_t inst = stm32_dma_alloc_instance(mask, name);

  int channel = inst == sa ? resource_id >> 24 : (resource_id >> 8) & 0xff;
  assert(channel < 8);
  reg_wr(DMA_SCR(inst), channel << 25);
  return inst;
}
