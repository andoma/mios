#pragma once

#include <mios/error.h>

typedef unsigned char stm32f4_dma_instance_t;

typedef enum {
  DMA_P_TO_M = 0,
  DMA_M_TO_P = 1,
} dma_direction_t;

stm32f4_dma_instance_t stm32f4_dma_alloc_fixed(int controller, int stream);

// IRQ_LEVEL_DMA must be masked
void stm32f4_dma_start(stm32f4_dma_instance_t instance,
                       void *maddr, uint32_t paddr, size_t items,
                       dma_direction_t direction, int channel);

// IRQ_LEVEL_DMA must be masked
error_t stm32f4_dma_wait(stm32f4_dma_instance_t instance);

