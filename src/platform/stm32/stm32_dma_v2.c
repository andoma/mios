// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file


#define DMA_CCRx(x)   (DMA_BASE((x) >> 3) + 0x08 + 0x14 * ((x) & 7))
#define DMA_CNDTRx(x) (DMA_BASE((x) >> 3) + 0x0c + 0x14 * ((x) & 7))
#define DMA_CPARx(x)  (DMA_BASE((x) >> 3) + 0x10 + 0x14 * ((x) & 7))
#define DMA_CMARx(x)  (DMA_BASE((x) >> 3) + 0x14 + 0x14 * ((x) & 7))

#define DMA_ISR(ctrl)  (DMA_BASE(ctrl) + 0x00)
#define DMA_IFCR(ctrl) (DMA_BASE(ctrl) + 0x04)

static const uint8_t irqmap[16] = {
  11,12,13,14,15,16,17,96,56,57,58,59,60,97,98,99
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
  task_wakeup_sched_locked(&ds->waitq, 0);
}


static stm32_dma_instance_t
stm32_dma_alloc_instance(int eligible, void (*cb)(stm32_dma_instance_t instance,
                                                  void *arg, error_t err),
                         void *arg, const char *name, int irq_level)
{
  int q = irq_forbid(IRQ_LEVEL_SWITCH);
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
      irq_level = IRQ_LEVEL_SCHED;
    }
    ds->cb = cb;
    ds->arg = arg;
    ds->err = 1; // Waiting
#ifdef CLK_DMA2
    clk_enable(i >= 8 ? CLK_DMA2 : CLK_DMA1);
#else
    clk_enable(CLK_DMA1);
#endif
    g_streams[i] = ds;
    if(irq_level != IRQ_LEVEL_NONE) {
      irq_enable(irqmap[i], irq_level);
    }
    irq_permit(q);
    printf("%s: Using DMA #%d/%d IRQ:%d\n", name, i >> 3, i & 7, irqmap[i]);
    return i;
  }
  panic("Out of DMA channels");
}

void
stm32_dma_set_paddr(stm32_dma_instance_t instance, uint32_t paddr)
{
  reg_wr(DMA_CPARx(instance), paddr);
}

void
stm32_dma_set_mem0(stm32_dma_instance_t instance, void *maddr)
{
  assert(maddr != NULL);
  reg_wr(DMA_CMARx(instance), (uint32_t)maddr);
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
                 stm32_dma_direction_t direction)
{
  uint32_t reg = 0;

  reg |= prio   << 12;
  reg |= msize  << 10;
  reg |= psize  << 8;
  reg |= minc   << 7;
  reg |= pinc   << 6;
  reg |= direction << 4;

  reg |= (1 << 3); // Transfer error interrupt enable
  reg |= (1 << 1); // Transfer complete interrupt enable
  reg_wr(DMA_CCRx(instance), reg);
}



void
stm32_dma_set_nitems(stm32_dma_instance_t instance, int nitems)
{
  reg_wr(DMA_CNDTRx(instance), nitems);
}



void
stm32_dma_start(stm32_dma_instance_t instance)
{
  reg_set_bit(DMA_CCRx(instance), 0);
}


void
stm32_dma_stop(stm32_dma_instance_t instance)
{
  reg_clr_bit(DMA_CCRx(instance), 0);
}


error_t
stm32_dma_wait(stm32_dma_instance_t instance)
{
  const int64_t deadline = clock_get_irq_blocked() + 1000000;
  dma_stream_t *ds = g_streams[instance];
  while(1) {
    if(ds->err == 1) {
      if(task_sleep_deadline(&ds->waitq, deadline, 0)) {
        reg_clr_bit(DMA_CCRx(instance), 0);
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
dma_irq(int channel)
{
  const uint32_t ctrl = channel >> 3;
  const uint32_t bits = (reg_rd(DMA_ISR(ctrl)) >> (channel * 4)) & 0xf;
  reg_wr(DMA_IFCR(ctrl), bits << (channel * 4));

  dma_stream_t *ds = g_streams[channel];
  if(ds == NULL)
    return;
  ds->cb(channel, ds->arg, bits & 0x8 ? ERR_DMA_ERROR : 0);
}


void irq_11(void) { dma_irq(0); }
void irq_12(void) { dma_irq(1); }
void irq_13(void) { dma_irq(2); }
void irq_14(void) { dma_irq(3); }
void irq_15(void) { dma_irq(4); }
void irq_16(void) { dma_irq(5); }
void irq_17(void) { dma_irq(6); }
void irq_96(void) { dma_irq(7); }
void irq_56(void) { dma_irq(8); }
void irq_57(void) { dma_irq(9); }
void irq_58(void) { dma_irq(10); }
void irq_59(void) { dma_irq(11); }
void irq_60(void) { dma_irq(12); }
void irq_97(void) { dma_irq(13); }
void irq_98(void) { dma_irq(14); }
void irq_99(void) { dma_irq(15); }
