#pragma once

#include <mios/error.h>

#include "platform/stm32/stm32_dma.h"

/*
 * BDMA (Basic DMA) driver for STM32H7 for low power domain D3 peripherals.
 *
 * BDMA is separate from DMA1/DMA2 and routes through DMAMUX2.
 * Buffers passed to stm32_bdma_set_mem0() must reside in D3 SRAM only.
 *
 * Mirrors the STM32 DMA API exactly, replace *dma with *bdma.
 */

typedef stm32_dma_instance_t stm32_bdma_instance_t;

stm32_bdma_instance_t stm32_bdma_alloc(int resource_id, const char *name);

void stm32_bdma_set_callback(stm32_bdma_instance_t inst,
                              void (*cb)(stm32_bdma_instance_t inst,
                                         uint32_t status, void *arg),
                              void *arg,
                              int irq_level,
                              uint32_t status_mask);

void stm32_bdma_config(stm32_bdma_instance_t inst,
                        int burst_mem,
                        int burst_periph,
                        int priority,
                        int data_size_mem,
                        int data_size_periph,
                        int mem_increment,
                        int periph_increment,
                        int circular,
                        int direction);

void stm32_bdma_set_paddr(stm32_bdma_instance_t inst, uint32_t paddr);
void stm32_bdma_set_mem0(stm32_bdma_instance_t inst, const void *maddr);
void stm32_bdma_set_nitems(stm32_bdma_instance_t inst, int n);
void stm32_bdma_start(stm32_bdma_instance_t inst);
void stm32_bdma_stop(stm32_bdma_instance_t inst);

