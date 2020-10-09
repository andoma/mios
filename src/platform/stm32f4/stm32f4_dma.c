#include <unistd.h>
#include <assert.h>
#include <stdio.h>

#include "task.h"
#include "irq.h"
#include "stm32f4.h"
#include "stm32f4_dma.h"

#include "mios.h"
#define DMA_BASE(x) (0x40026000 + (x) * 0x400)
#define DMA_ISR(x)  (0x00 + (x) * 4)
#define DMA_IFCR(x) (0x08 + (x) * 4)

#define DMA_SCR(x)   (0x10 + 0x18 * (x))
#define DMA_SNDTR(x) (0x14 + 0x18 * (x))
#define DMA_SPAR(x)  (0x18 + 0x18 * (x))
#define DMA_SM0AR(x) (0x1c + 0x18 * (x))
#define DMA_SFCR(x)  (0x24 + 0x18 * (x))


static const uint8_t irqmap[16] = {
  11,12,13,14,15,16,17,47,56,57,58,59,60,68,69,70
};

static uint8_t isrresult[16];

static struct task_queue waitq[16];


static int
stm32f4_dma_alloc_instance(int instance)
{
  TAILQ_INIT(&waitq[instance]);
  isrresult[instance] = 0;
  irq_enable(irqmap[instance], IRQ_LEVEL_DMA);
  return instance;
}

stm32f4_dma_instance_t
stm32f4_dma_alloc_fixed(int controller, int stream)
{
  reg_set_bit(RCC_AHB1ENR, 21 + controller);  // CLK ENABLE: DMA
  return stm32f4_dma_alloc_instance(controller * 8 + stream);
}


static uint8_t dma_dump_station;

void
stm32f4_dma_start(stm32f4_dma_instance_t instance,
                  void *maddr, uint32_t paddr, size_t items,
                  dma_direction_t direction, int channel)
{
  const uint8_t stream = instance & 7;
  const uint8_t controller = instance >> 3;
  const uint32_t base = DMA_BASE(controller);
  assert(isrresult[instance] == 0);

  reg_wr(base + DMA_SCR(stream),
         (channel << 25) |
         ((!!maddr) << 10) | // Memory increment mode
         (direction << 6) |
         (1 << 4) | // Interrupt enable
         0);

  reg_wr(base + DMA_SPAR(stream),  (uint32_t)paddr);
  reg_wr(base + DMA_SM0AR(stream), (uint32_t)(maddr ?: &dma_dump_station));
  reg_wr(base + DMA_SNDTR(stream), items);
  reg_set_bit(base + DMA_SCR(stream), 0);
}


error_t
stm32f4_dma_wait(stm32f4_dma_instance_t instance)
{
  const uint8_t stream = instance & 7;
  const uint8_t controller = instance >> 3;
  const uint32_t base = DMA_BASE(controller);

  while(1) {
    const uint8_t bits = isrresult[instance];
    if(bits == 0) {
      if(task_sleep_delta(&waitq[instance], 100000, 0)) {
        return ERR_TIMEOUT;
      }
      continue;
    }

    reg_clr_bit(base + DMA_SCR(stream), 0);

    isrresult[instance] = 0;
    return bits & 0x20 ? ERR_OK : ERR_DMA_ERROR;
  }
}

static void
dma_irq(int instance, int bits)
{
  isrresult[instance] = bits;
  task_wakeup(&waitq[instance], 0);
}

#define GET_ISR(controller, hi, offset)                                \
  const uint32_t base = DMA_BASE(controller);                          \
  const uint32_t bits = (reg_rd(base + DMA_ISR(hi)) >> offset) & 0x3f; \
  reg_wr(base + DMA_IFCR(hi), 0x3f << offset);                         \


void irq_11(void) { GET_ISR(0, 0, 0)  dma_irq( 0, bits); }
void irq_12(void) { GET_ISR(0, 0, 6)  dma_irq( 1, bits); }
void irq_13(void) { GET_ISR(0, 0, 16) dma_irq( 2, bits); }
void irq_14(void) { GET_ISR(0, 0, 22) dma_irq( 3, bits); }
void irq_15(void) { GET_ISR(0, 1, 0)  dma_irq( 4, bits); }
void irq_16(void) { GET_ISR(0, 1, 6)  dma_irq( 5, bits); }
void irq_17(void) { GET_ISR(0, 1, 16) dma_irq( 6, bits); }
void irq_47(void) { GET_ISR(0, 1, 22) dma_irq( 7, bits); }
void irq_56(void) { GET_ISR(1, 0,  0) dma_irq( 8, bits); }
void irq_57(void) { GET_ISR(1, 0,  6) dma_irq( 9, bits); }
void irq_58(void) { GET_ISR(1, 0, 16) dma_irq(10, bits); }
void irq_59(void) { GET_ISR(1, 0, 22) dma_irq(11, bits); }
void irq_60(void) { GET_ISR(1, 1,  0) dma_irq(12, bits); }
void irq_68(void) { GET_ISR(1, 1,  6) dma_irq(13, bits); }
void irq_69(void) { GET_ISR(1, 1, 16) dma_irq(14, bits); }
void irq_70(void) { GET_ISR(1, 1, 22) dma_irq(15, bits); }
