// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file


#define DMA_CCRx(x)   (DMA_BASE((x) >> 3) + 0x08 + 0x14 * ((x) & 7))
#define DMA_CNDTRx(x) (DMA_BASE((x) >> 3) + 0x0c + 0x14 * ((x) & 7))
#define DMA_CPARx(x)  (DMA_BASE((x) >> 3) + 0x10 + 0x14 * ((x) & 7))
#define DMA_CMARx(x)  (DMA_BASE((x) >> 3) + 0x14 + 0x14 * ((x) & 7))

#define DMA_ISR(ctrl)  (DMA_BASE(ctrl) + 0x00)
#define DMA_IFCR(ctrl) (DMA_BASE(ctrl) + 0x04)

typedef struct dma_stream {

  dma_cb_t *cb;
  void *arg;

  task_waitable_t waitq;
  uint32_t status;

} dma_stream_t;

#ifdef CLK_DMA2
#define NUM_DMA_STREAMS 16
#else
#define NUM_DMA_STREAMS 8
#endif

static dma_stream_t *g_streams[NUM_DMA_STREAMS];



static void
wakeup_cb(stm32_dma_instance_t instance, uint32_t status, void *arg)
{
  dma_stream_t *ds = arg;
  ds->status |= status;
  task_wakeup_sched_locked(&ds->waitq, 0);
}


static stm32_dma_instance_t
stm32_dma_alloc_instance(int eligible, const char *name)
{
  int q = irq_forbid(IRQ_LEVEL_SWITCH);
  for(int i = 0; i < NUM_DMA_STREAMS; i++) {
    if(!((1 << i) & eligible))
      continue;
    if(g_streams[i])
      continue;

    dma_stream_t *ds = calloc(1, sizeof(dma_stream_t));
#ifdef CLK_DMA2
    clk_enable(i >= 8 ? CLK_DMA2 : CLK_DMA1);
#else
    clk_enable(CLK_DMA1);
#endif
    g_streams[i] = ds;
    irq_permit(q);
    printf("%s: Using DMA #%d/%d\n", name, i >> 3, i & 7);
    return i;
  }
  panic("Out of DMA channels");
}


void
stm32_dma_set_callback(stm32_dma_instance_t instance,
                       dma_cb_t *cb,
                       void *arg,
                       int irq_level,
                       uint32_t mask)
{
  dma_stream_t *ds = g_streams[instance];
  ds->cb = cb;
  ds->arg = arg;

  uint32_t bits = 0;
  if(mask & DMA_STATUS_FULL_XFER)
    bits |= (1 << 0);
  if(mask & DMA_STATUS_HALF_XFER)
    bits |= (1 << 1);
  if(mask & DMA_STATUS_XFER_ERROR)
    bits |= (1 << 2);

  reg_set_bits(DMA_CCRx(instance), 1, 3, bits);

  irq_enable(dma_irqmap[instance], irq_level);
}


void
stm32_dma_make_waitable(stm32_dma_instance_t instance, const char *name)
{
  dma_stream_t *ds = g_streams[instance];
  task_waitable_init(&ds->waitq, name);
  ds->status = 0;
  ds->cb = wakeup_cb;
  ds->arg = ds;

  reg_set_bits(DMA_CCRx(instance), 1, 3, 0b101);

  irq_enable(dma_irqmap[instance], IRQ_LEVEL_SCHED);
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
                 stm32_dma_circular_t circ,
                 stm32_dma_direction_t direction)
{
  uint32_t reg = 0;

  reg |= prio   << 12;
  reg |= msize  << 10;
  reg |= psize  << 8;
  reg |= minc   << 7;
  reg |= pinc   << 6;
  reg |= circ   << 5;
  reg |= direction << 4;
  uint32_t v = reg_rd(DMA_CCRx(instance));
  v &= 0xffff000f;
  v |= reg;
  reg_wr(DMA_CCRx(instance), v);
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
  while(reg_get_bit(DMA_CCRx(instance), 0)) {}
}


error_t
stm32_dma_wait(stm32_dma_instance_t instance)
{
  const int64_t start = clock_get_irq_blocked();
  const int64_t deadline = start + 1000000;
  dma_stream_t *ds = g_streams[instance];
  while(1) {
    if(ds->status & DMA_STATUS_XFER_ERROR) {
      ds->status = 0;
      return ERR_DMA_XFER;
    }
    if(ds->status & DMA_STATUS_FULL_XFER) {
      ds->status = 0;
      return 0;
    }

    if(task_sleep_deadline(&ds->waitq, deadline)) {
      stm32_dma_stop(instance);
      ds->status = 0;
      return ERR_TIMEOUT;
    }
  }
}

static void __attribute__((noinline))
dma_irq(int channel)
{
  const uint32_t ctrl = channel >> 3;
  const uint32_t bits = (reg_rd(DMA_ISR(ctrl)) >> (channel * 4)) & 0xe;
  reg_wr(DMA_IFCR(ctrl), bits << (channel * 4));

  dma_stream_t *ds = g_streams[channel];
  if(ds == NULL || ds->cb == NULL)
    return;

  uint32_t status =
    (bits & 0x8) |        // ERROR
    ((bits & 0x4) << 2) | // HALF
    ((bits & 0x2) << 4);  // FULL

  ds->cb(channel, status, ds->arg);
}

