#pragma once

#include "platform/stm32/stm32_dma.h"

stm32_dma_instance_t stm32_dma_alloc(uint32_t resource_id, const char *name);
