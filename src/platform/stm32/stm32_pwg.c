// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file

#include <mios/pwg.h>

#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>

#include "cache.h"

#define PWG_BUF_SIZE 64
#define PWG_HALF_SIZE (PWG_BUF_SIZE / 2)

typedef struct stm32_pwg {

  // DMA buffer must be cache-line aligned and not share cache lines
  // with other fields, since we use dcache_op(DCACHE_CLEAN) on it.
  uint32_t buf[PWG_BUF_SIZE];

  uint32_t timer_base;
  uint32_t gpio_bsrr_addr;
  stm32_dma_instance_t dma;
  task_waitable_t waitq;
  pwg_fill_cb fill_cb;
  void *opaque;
  uint8_t active_half;
  uint8_t running;

} stm32_pwg_t;


static void
pwg_dma_cb(stm32_dma_instance_t inst, uint32_t status, void *arg)
{
  stm32_pwg_t *pwg = arg;

  if(status & DMA_STATUS_HALF_XFER)
    pwg->active_half = 1;

  if(status & DMA_STATUS_FULL_XFER)
    pwg->active_half = 0;

  task_wakeup_sched_locked(&pwg->waitq, 0);
}


static void *
pwg_thread(void *arg)
{
  stm32_pwg_t *pwg = arg;

  pwg->fill_cb(pwg->opaque, &pwg->buf[0], PWG_HALF_SIZE);
  pwg->fill_cb(pwg->opaque, &pwg->buf[PWG_HALF_SIZE], PWG_HALF_SIZE);
  dcache_op(pwg->buf, sizeof(pwg->buf), DCACHE_CLEAN);

  stm32_dma_start(pwg->dma);
  reg_wr(pwg->timer_base + TIMx_CR1, 1);

  while(pwg->running) {

    int q = irq_forbid(IRQ_LEVEL_SCHED);
    task_sleep_sched_locked(&pwg->waitq);
    uint8_t active = pwg->active_half;
    irq_permit(q);

    uint32_t *half = active ?
      &pwg->buf[0] : &pwg->buf[PWG_HALF_SIZE];
    pwg->fill_cb(pwg->opaque, half, PWG_HALF_SIZE);
    dcache_op(half, PWG_HALF_SIZE * sizeof(uint32_t), DCACHE_CLEAN);
  }

  reg_wr(pwg->timer_base + TIMx_CR1, 0);
  stm32_dma_stop(pwg->dma);
  return NULL;
}


static stm32_pwg_t *
stm32_pwg_init(uint32_t timer_base,
               uint16_t timer_clkid,
               uint32_t dma_resource_id,
               uint32_t gpio_bsrr_addr,
               uint32_t frequency,
               pwg_fill_cb fill_cb,
               void *opaque)
{
  stm32_pwg_t *pwg = xalloc(sizeof(stm32_pwg_t), CACHE_LINE_SIZE,
                            MEM_TYPE_DMA | MEM_CLEAR);

  pwg->timer_base = timer_base;
  pwg->gpio_bsrr_addr = gpio_bsrr_addr;
  pwg->fill_cb = fill_cb;
  pwg->opaque = opaque;
  pwg->running = 1;

  task_waitable_init(&pwg->waitq, "pwg");

  // Configure timer

  clk_enable(timer_clkid);

  uint32_t tclk = clk_get_freq(timer_clkid);
  uint32_t div = tclk / frequency;
  uint32_t psc = 0;

  while(div > 65536) {
    psc++;
    div = tclk / (frequency * (psc + 1));
  }

  reg_wr(timer_base + TIMx_PSC, psc);
  reg_wr(timer_base + TIMx_ARR, div - 1);
  reg_wr(timer_base + TIMx_EGR, 1);  // Generate update to load PSC/ARR
  reg_wr(timer_base + TIMx_DIER, 1 << 8);  // UDE - Update DMA Enable

  // Configure DMA

  pwg->dma = stm32_dma_alloc(dma_resource_id, "pwg");

  stm32_dma_config(pwg->dma,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_PRIO_HIGH,
                   STM32_DMA_32BIT,
                   STM32_DMA_32BIT,
                   STM32_DMA_INCREMENT,
                   STM32_DMA_FIXED,
                   STM32_DMA_CIRCULAR,
                   STM32_DMA_M_TO_P);

  stm32_dma_set_paddr(pwg->dma, gpio_bsrr_addr);
  stm32_dma_set_mem0(pwg->dma, pwg->buf);
  stm32_dma_set_nitems(pwg->dma, PWG_BUF_SIZE);

  stm32_dma_set_callback(pwg->dma, pwg_dma_cb, pwg, IRQ_LEVEL_SCHED,
                         DMA_STATUS_HALF_XFER | DMA_STATUS_FULL_XFER);

  thread_create(pwg_thread, pwg, 512, "pwg", TASK_DETACHED, 20);

  return pwg;
}


void
stm32_pwg_stop(stm32_pwg_t *pwg)
{
  pwg->running = 0;
  task_wakeup(&pwg->waitq, 0);
}
