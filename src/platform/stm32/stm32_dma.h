#pragma once

#include <mios/error.h>

typedef uint8_t stm32_dma_instance_t;

#define STM32_DMA_INSTANCE_NONE 0xff

typedef enum {
  STM32_DMA_BURST_NONE   = 0,
  STM32_DMA_BURST_INCR4  = 1,
  STM32_DMA_BURST_INCR8  = 2,
  STM32_DMA_BURST_INCR16 = 3,
} stm32_dma_burst_t;

typedef enum {
  STM32_DMA_PRIO_LOW       = 0,
  STM32_DMA_PRIO_MID       = 1,
  STM32_DMA_PRIO_HIGH      = 2,
  STM32_DMA_PRIO_VERY_HIGH = 3,
} stm32_dma_prio_t;

typedef enum {
  STM32_DMA_8BIT  = 0,
  STM32_DMA_16BIT = 1,
  STM32_DMA_32BIT = 2,
} stm32_dma_data_size_t;

typedef enum {
  STM32_DMA_FIXED     = 0,
  STM32_DMA_INCREMENT = 1,
} stm32_dma_incr_mode_t;

typedef enum {
  STM32_DMA_P_TO_M = 0,
  STM32_DMA_M_TO_P = 1,
} stm32_dma_direction_t;

void stm32_dma_config(stm32_dma_instance_t instance,
                      stm32_dma_burst_t mburst,
                      stm32_dma_burst_t pburst,
                      stm32_dma_prio_t prio,
                      stm32_dma_data_size_t msize,
                      stm32_dma_data_size_t psize,
                      stm32_dma_incr_mode_t minc,
                      stm32_dma_incr_mode_t pinc,
                      stm32_dma_direction_t direction);

void stm32_dma_set_paddr(stm32_dma_instance_t instance, uint32_t paddr);

void stm32_dma_set_mem0(stm32_dma_instance_t instance, void *maddr);

void stm32_dma_set_mem1(stm32_dma_instance_t instance, void *maddr);

void stm32_dma_set_nitems(stm32_dma_instance_t instance, int nitems);

void stm32_dma_start(stm32_dma_instance_t instance);

void stm32_dma_stop(stm32_dma_instance_t instance);

error_t stm32_dma_wait(stm32_dma_instance_t instance);

stm32_dma_instance_t stm32_dma_alloc(uint32_t resource_id,
                                     void (*cb)(stm32_dma_instance_t,
                                                void *arg, error_t err),
                                     void *arg, const char *name,
                                     int irq_level);
