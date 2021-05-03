#pragma once

#include <mios/error.h>

#include "platform/stm32/stm32_dma.h"

stm32_dma_instance_t stm32h7_dma_alloc(int resource_id,
                                       void (*cb)(stm32_dma_instance_t instance,
                                                  void *arg, error_t err),
                                       void *arg, const char *name,
                                       int irq_level);
