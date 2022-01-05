#pragma once

#include <mios/error.h>

#include "platform/stm32/stm32_dma.h"

stm32_dma_instance_t stm32h7_dma_alloc(int resource_id, const char *name);
