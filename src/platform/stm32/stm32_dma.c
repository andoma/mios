// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file


#define DMA_SCR(x)   (DMA_BASE((x) >> 3) + 0x10 + 0x18 * ((x) & 7))
#define DMA_SNDTR(x) (DMA_BASE((x) >> 3) + 0x14 + 0x18 * ((x) & 7))
#define DMA_SPAR(x)  (DMA_BASE((x) >> 3) + 0x18 + 0x18 * ((x) & 7))
#define DMA_SM0AR(x) (DMA_BASE((x) >> 3) + 0x1c + 0x18 * ((x) & 7))
#define DMA_SM1AR(x) (DMA_BASE((x) >> 3) + 0x20 + 0x18 * ((x) & 7))
#define DMA_SFCR(x)  (DMA_BASE((x) >> 3) + 0x24 + 0x18 * ((x) & 7))


static const uint8_t irqmap[16] = {
  11,12,13,14,15,16,17,47,56,57,58,59,60,68,69,70
};


typedef struct dma_stream {

  void (*cb)(stm32_dma_instance_t instance, void *arg, error_t err);
  void *arg;

  task_waitable_t waitq;
  error_t err;

} dma_stream_t;


static dma_stream_t *g_streams[16];



static void
wakeup_cb(stm32_dma_instance_t instance, void *arg, error_t err)
{
  dma_stream_t *ds = arg;
  ds->err = err;
  task_wakeup(&ds->waitq, 0);
}


static stm32_dma_instance_t
stm32_dma_alloc(int eligible, void (*cb)(stm32_dma_instance_t instance,
                                         void *arg, error_t err),
                void *arg, const char *name)
{
  int q = irq_forbid(IRQ_LEVEL_DMA);
  for(int i = 0; i < 16; i++) {
    if(!((1 << i) & eligible))
      continue;
    if(g_streams[i])
      continue;

    dma_stream_t *ds = malloc(sizeof(dma_stream_t));
    task_waitable_init(&ds->waitq, name);
    if(cb == NULL) {
      cb = wakeup_cb;
      arg = ds;
    }
    ds->cb = cb;
    ds->arg = arg;
    ds->err = 1; // Waiting
    clk_enable(i >= 8 ? CLK_DMA2 : CLK_DMA1);
    g_streams[i] = ds;
    irq_enable(irqmap[i], IRQ_LEVEL_DMA);
    irq_permit(q);
    printf("DMA ch%d allocated by %s\n", i, name);
    return i;
  }
  panic("Out of DMA channels");
}

void
stm32_dma_set_paddr(stm32_dma_instance_t instance, uint32_t paddr)
{
  reg_wr(DMA_SPAR(instance), paddr);
}

void
stm32_dma_set_mem0(stm32_dma_instance_t instance, void *maddr)
{
  assert(maddr != NULL);
  reg_wr(DMA_SM0AR(instance), (uint32_t)maddr);
}

void
stm32_dma_set_mem1(stm32_dma_instance_t instance, void *maddr)
{
  assert(maddr != NULL);
  reg_wr(DMA_SM1AR(instance), (uint32_t)maddr);
}

void
stm32_dma_config(stm32_dma_instance_t instance,
                 stm32_dma_burst_t mburst,
                 stm32_dma_burst_t pburst,
                 stm32_dma_prio_t prio,
                 stm32_dma_data_size_t msize,
                 stm32_dma_data_size_t psize,
                 stm32_dma_incr_mode_t minc,
                 stm32_dma_incr_mode_t pinc,
                 stm32_dma_direction_t dir)
{
  uint32_t reg = reg_rd(DMA_SCR(instance));

  reg &= 0x00fffffe;

  reg |= mburst << 23;
  reg |= pburst << 21;
  reg |= prio   << 16;
  reg |= msize  << 13;
  reg |= psize  << 11;
  reg |= minc   << 10;
  reg |= pinc   << 9;
  reg |= dir    << 6;
  reg |= 1 << 4;
  reg |= 1 << 2;
  reg |= 1 << 1;
  reg_wr(DMA_SCR(instance), reg);
}


void
stm32_dma_set_nitems(stm32_dma_instance_t instance, int nitems)
{
  reg_wr(DMA_SNDTR(instance), nitems);
}



void
stm32_dma_start(stm32_dma_instance_t instance)
{
  reg_set_bit(DMA_SCR(instance), 0);
}



error_t
stm32_dma_wait(stm32_dma_instance_t instance)
{
  const int64_t deadline = clock_get_irq_blocked() + 100000;
  dma_stream_t *ds = g_streams[instance];

  while(1) {
    if(ds->err == 1) {
      if(task_sleep_deadline(&ds->waitq, deadline, 0)) {
        reg_clr_bit(DMA_SCR(instance), 0);
        return ERR_TIMEOUT;
      }
      continue;
    }
    int ret = ds->err;
    ds->err = 1;
    return ret;
  }
}

static void __attribute__((noinline))
dma_irq(int instance, int bits)
{
  dma_stream_t *ds = g_streams[instance];
  if(ds == NULL)
    return;

  ds->cb(instance, ds->arg, bits & 0xc ? ERR_DMA_ERROR : 0);
}

#define DMA_ISR(hi)  (0x00 + (hi) * 4)
#define DMA_IFCR(hi) (0x08 + (hi) * 4)


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
