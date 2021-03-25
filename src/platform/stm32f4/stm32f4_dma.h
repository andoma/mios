#pragma once

#include "platform/stm32/stm32_dma.h"

stm32_dma_instance_t stm32f4_dma_alloc_fixed(int controller, int stream,
                                             int channel,
                                             void (*cb)(stm32_dma_instance_t,
                                                        void *arg, error_t err),
                                             void *arg, const char *name);

