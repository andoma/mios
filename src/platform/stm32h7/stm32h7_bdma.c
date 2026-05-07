#include <string.h>
#include <assert.h>

#include <mios/task.h>

#include "irq.h"
#include "stm32h7_clk.h"
#include "stm32h7_bdma.h"

/* -------------------------------------------------------------------------
 * Register map — BDMA 0x58025400, DMAMUX2 0x58025800
 * (RM0433 §17, RM0468 §17)
 * ------------------------------------------------------------------------- */

#define BDMA_BASE        0x58025400
#define BDMA_ISR         (BDMA_BASE + 0x00)  /* interrupt status (ro) */
#define BDMA_IFCR        (BDMA_BASE + 0x04)  /* interrupt flag clear  */

/* Channel n register block starts at 0x08 + n*0x14 */
#define BDMA_CH(n)       (BDMA_BASE + 0x08 + (n) * 0x14)
#define BDMA_CCR(n)      (BDMA_CH(n) + 0x00)  /* configuration      */
#define BDMA_CNDTR(n)    (BDMA_CH(n) + 0x04)  /* number of data     */
#define BDMA_CPAR(n)     (BDMA_CH(n) + 0x08)  /* peripheral address */
#define BDMA_CM0AR(n)    (BDMA_CH(n) + 0x0C)  /* memory 0 address   */
#define BDMA_CM1AR(n)    (BDMA_CH(n) + 0x10)  /* memory 1 address   */

/* BDMA_CCR bit definitions */
#define CCR_EN           (1 <<  0)  /* channel enable                        */
#define CCR_TCIE         (1 <<  1)  /* transfer complete interrupt enable     */
#define CCR_HTIE         (1 <<  2)  /* half transfer interrupt enable         */
#define CCR_TEIE         (1 <<  3)  /* transfer error interrupt enable        */
#define CCR_DIR          (1 <<  4)  /* direction: 0=P→M, 1=M→P               */
#define CCR_CIRC         (1 <<  5)  /* circular mode                          */
#define CCR_PINC         (1 <<  6)  /* peripheral address increment           */
#define CCR_MINC         (1 <<  7)  /* memory address increment               */
/* PSIZE [9:8]: peripheral data width  — 0=8b 1=16b 2=32b                    */
/* MSIZE [11:10]: memory data width    — 0=8b 1=16b 2=32b                    */
/* PL [13:12]: priority level          — 0=low 1=med 2=high 3=very high      */
#define CCR_PSIZE(x)     (((x) & 0x3) <<  8)
#define CCR_MSIZE(x)     (((x) & 0x3) << 10)
#define CCR_PL(x)        (((x) & 0x3) << 12)

/* ISR/IFCR per-channel bit positions */
#define ISR_GIF(n)       (1 << ((n) * 4 + 0))  /* global (clears all below) */
#define ISR_TCIF(n)      (1 << ((n) * 4 + 1))  /* transfer complete         */
#define ISR_HTIF(n)      (1 << ((n) * 4 + 2))  /* half transfer             */
#define ISR_TEIF(n)      (1 << ((n) * 4 + 3))  /* transfer error            */

/* DMAMUX2 — routes BDMA request IDs to channels
 * (RM0433 Table 121, RM0468 Table 121) */
#define DMAMUX2_BASE     0x58025800
#define DMAMUX2_CxCR(n)  (DMAMUX2_BASE + 4 * (n))

/* -------------------------------------------------------------------------
 * IRQ numbers — BDMA channels 0-7, positions 129-136.
 * Identical across all STM32H7 variants (RM0433 Table 10, RM0468 Table 10).
 * ------------------------------------------------------------------------- */
#define BDMA_NCHANNELS   8
static const uint8_t bdma_irqnums[BDMA_NCHANNELS] = {
  129, 130, 131, 132, 133, 134, 135, 136
};

/* -------------------------------------------------------------------------
 * Channel state
 * ------------------------------------------------------------------------- */
typedef struct {
  void     (*cb)(stm32_bdma_instance_t inst, uint32_t status, void *arg);
  void      *cb_arg;
  int        irq_level;
  uint32_t   status_mask;
  uint32_t   ccr;     /* configured CCR value, written on start (no EN/IE bits) */
} bdma_chan_t;

static bdma_chan_t  g_chans[BDMA_NCHANNELS];
static uint8_t     g_used;      /* bitmask of allocated channels */

/* -------------------------------------------------------------------------
 * IRQ handlers — one per channel
 * ------------------------------------------------------------------------- */
static void
bdma_irq(int ch)
{
  const uint32_t isr = reg_rd(BDMA_ISR);
  uint32_t status = 0;

  if(isr & ISR_TCIF(ch)) status |= DMA_STATUS_FULL_XFER;
  if(isr & ISR_HTIF(ch)) status |= DMA_STATUS_HALF_XFER;
  if(isr & ISR_TEIF(ch)) status |= DMA_STATUS_XFER_ERROR;

  reg_wr(BDMA_IFCR, ISR_GIF(ch));  /* clear all flags for this channel */

  const bdma_chan_t *c = &g_chans[ch];
  const uint32_t pending = status & c->status_mask;
  if(c->cb && pending)
    c->cb((stm32_bdma_instance_t)ch, pending, c->cb_arg);
}

/* Generate a named ISR stub for each channel */
#define BDMA_ISR_STUB(n) \
  static void bdma_ch##n##_irq(void) { bdma_irq(n); }

BDMA_ISR_STUB(0)
BDMA_ISR_STUB(1)
BDMA_ISR_STUB(2)
BDMA_ISR_STUB(3)
BDMA_ISR_STUB(4)
BDMA_ISR_STUB(5)
BDMA_ISR_STUB(6)
BDMA_ISR_STUB(7)

static void (* const bdma_irq_handlers[BDMA_NCHANNELS])(void) = {
  bdma_ch0_irq, bdma_ch1_irq, bdma_ch2_irq, bdma_ch3_irq,
  bdma_ch4_irq, bdma_ch5_irq, bdma_ch6_irq, bdma_ch7_irq,
};

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

stm32_bdma_instance_t
stm32_bdma_alloc(int resource_id, const char *name)
{
  clk_enable(CLK_BDMA);

  int q = irq_forbid(IRQ_LEVEL_SWITCH);

  int ch = -1;
  for(int i = 0; i < BDMA_NCHANNELS; i++) {
    if(!(g_used & (1 << i))) {
      ch = i;
      g_used |= (1 << i);
      break;
    }
  }

  irq_permit(q);

  if(ch < 0)
    panic("stm32_bdma_alloc: no free channels (requested for '%s')", name);

  memset(&g_chans[ch], 0, sizeof(g_chans[ch]));

  /* disable channel and clear any stale flags before routing */
  reg_wr(BDMA_CCR(ch), 0);
  reg_wr(BDMA_IFCR, ISR_GIF(ch));

  /* route DMAMUX2 request to this channel */
  reg_wr(DMAMUX2_CxCR(ch), resource_id);

  return (stm32_bdma_instance_t)ch;
}

void
stm32_bdma_set_callback(stm32_bdma_instance_t inst,
                         void (*cb)(stm32_bdma_instance_t inst,
                                    uint32_t status, void *arg),
                         void *arg,
                         int irq_level,
                         uint32_t status_mask)
{
  const int ch = (int)inst;
  bdma_chan_t *c = &g_chans[ch];

  c->cb          = cb;
  c->cb_arg      = arg;
  c->irq_level   = irq_level;
  c->status_mask = status_mask;

  irq_enable_fn(bdma_irqnums[ch], irq_level, bdma_irq_handlers[ch]);
}

void
stm32_bdma_config(stm32_bdma_instance_t inst,
                   int burst_mem,       /* unused — BDMA has no burst support */
                   int burst_periph,    /* unused — BDMA has no burst support */
                   int priority,
                   int data_size_mem,
                   int data_size_periph,
                   int mem_increment,
                   int periph_increment,
                   int circular,
                   int direction)
{
  (void)burst_mem;
  (void)burst_periph;

  uint32_t ccr = 0;

  if(direction        == STM32_DMA_M_TO_P)   ccr |= CCR_DIR;
  if(circular         == STM32_DMA_CIRCULAR)  ccr |= CCR_CIRC;
  if(mem_increment    == STM32_DMA_INCREMENT) ccr |= CCR_MINC;
  if(periph_increment == STM32_DMA_INCREMENT) ccr |= CCR_PINC;

  ccr |= CCR_PSIZE(data_size_periph);
  ccr |= CCR_MSIZE(data_size_mem);
  ccr |= CCR_PL(priority);

  g_chans[(int)inst].ccr = ccr;
}

void
stm32_bdma_set_paddr(stm32_bdma_instance_t inst, uint32_t paddr)
{
  reg_wr(BDMA_CPAR((int)inst), paddr);
}

void
stm32_bdma_set_mem0(stm32_bdma_instance_t inst, const void *maddr)
{
  reg_wr(BDMA_CM0AR((int)inst), (uint32_t)(uintptr_t)maddr);
}

void
stm32_bdma_set_nitems(stm32_bdma_instance_t inst, int n)
{
  reg_wr(BDMA_CNDTR((int)inst), (uint32_t)n);
}

void
stm32_bdma_start(stm32_bdma_instance_t inst)
{
  const int ch = (int)inst;
  const bdma_chan_t *c = &g_chans[ch];

  /* build interrupt enable mask from status_mask */
  uint32_t ie = 0;
  if(c->status_mask & DMA_STATUS_FULL_XFER)  ie |= CCR_TCIE;
  if(c->status_mask & DMA_STATUS_HALF_XFER)  ie |= CCR_HTIE;
  if(c->status_mask & DMA_STATUS_XFER_ERROR) ie |= CCR_TEIE;

  reg_wr(BDMA_IFCR, ISR_GIF(ch));
  reg_wr(BDMA_CCR(ch), c->ccr | ie | CCR_EN);
}

void
stm32_bdma_stop(stm32_bdma_instance_t inst)
{
  const int ch = (int)inst;

  reg_wr(BDMA_CCR(ch), 0);           /* clear EN and all interrupt enables */
  reg_wr(BDMA_IFCR, ISR_GIF(ch));    /* clear any pending flags            */
}

