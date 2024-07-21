// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file


#define DMA_SCR(x)   (DMA_BASE((x) >> 3) + 0x10 + 0x18 * ((x) & 7))
#define DMA_SNDTR(x) (DMA_BASE((x) >> 3) + 0x14 + 0x18 * ((x) & 7))
#define DMA_SPAR(x)  (DMA_BASE((x) >> 3) + 0x18 + 0x18 * ((x) & 7))
#define DMA_SM0AR(x) (DMA_BASE((x) >> 3) + 0x1c + 0x18 * ((x) & 7))
#define DMA_SM1AR(x) (DMA_BASE((x) >> 3) + 0x20 + 0x18 * ((x) & 7))
#define DMA_SFCR(x)  (DMA_BASE((x) >> 3) + 0x24 + 0x18 * ((x) & 7))

#define DMA_ISR(hi)  (0x00 + (hi) * 4)
#define DMA_IFCR(hi) (0x08 + (hi) * 4)


static const uint8_t irqmap[16] = {
  11,12,13,14,15,16,17,47,56,57,58,59,60,68,69,70
};


typedef struct dma_stream {

  dma_cb_t *cb;
  void *arg;

  task_waitable_t waitq;
  uint32_t status;

} dma_stream_t;


static dma_stream_t *g_streams[16];



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
  for(int i = 0; i < 16; i++) {
    if(!((1 << i) & eligible))
      continue;
    if(g_streams[i])
      continue;

    dma_stream_t *ds = malloc(sizeof(dma_stream_t));
    clk_enable(i >= 8 ? CLK_DMA2 : CLK_DMA1);
    g_streams[i] = ds;
    irq_permit(q);
    printf("%s: Using DMA #%d/%d\n", name, i >> 3, i & 7);
    return i;
  }
  panic("Out of DMA channels");
}


void
stm32_dma_set_callback(stm32_dma_instance_t i, dma_cb_t *cb,  void *arg,
                       int irq_level, uint32_t status_mask)
{
  dma_stream_t *ds = g_streams[i];
  ds->cb = cb;
  ds->arg = arg;
  irq_enable(irqmap[i], irq_level);

  const uint32_t ie = (status_mask >> 1) & 0x1e;
  reg_set_bits(DMA_SCR(i), 0, 5, ie);
}


void
stm32_dma_make_waitable(stm32_dma_instance_t i, const char *name)
{
  dma_stream_t *ds = g_streams[i];
  task_waitable_init(&ds->waitq, name);
  ds->status = 0;
  ds->cb = wakeup_cb;
  ds->arg = ds;
  irq_enable(irqmap[i], IRQ_LEVEL_SCHED);
  reg_set_bits(DMA_SCR(i), 0, 5, 0b10110);
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
stm32_dma_config_set_reg(stm32_dma_instance_t instance, uint32_t val)
{
  uint32_t reg = reg_rd(DMA_SCR(instance));
  reg &= 0xfe00001f;
  reg |= val;
  reg_wr(DMA_SCR(instance), reg);
}

uint32_t
stm32_dma_config_make_reg(stm32_dma_burst_t mburst,
                          stm32_dma_burst_t pburst,
                          stm32_dma_prio_t prio,
                          stm32_dma_data_size_t msize,
                          stm32_dma_data_size_t psize,
                          stm32_dma_incr_mode_t minc,
                          stm32_dma_incr_mode_t pinc,
                          stm32_dma_circular_t circular,
                          stm32_dma_direction_t direction)
{
  uint32_t reg = 0;

  reg |= mburst << 23;
  reg |= pburst << 21;
  reg |= prio   << 16;
  reg |= msize  << 13;
  reg |= psize  << 11;
  reg |= minc   << 10;
  reg |= pinc   << 9;
  reg |= circular << 8;
  reg |= direction << 6;
  return reg;
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


void
stm32_dma_stop(stm32_dma_instance_t instance)
{
  reg_clr_bit(DMA_SCR(instance), 0);
  while(reg_get_bit(DMA_SCR(instance), 0)) {}

  const uint32_t base = DMA_BASE(instance >> 3);
  const int hi = (instance >> 2) & 1;
  const uint8_t offset = (const uint8_t []){0,6,16,22}[instance & 3];
  reg_wr(base + DMA_IFCR(hi), 0x3f << offset);
}

error_t
stm32_dma_wait(stm32_dma_instance_t instance)
{
  const int64_t deadline = clock_get_irq_blocked() + 100000;
  dma_stream_t *ds = g_streams[instance];

  while(1) {
    if(ds->status == 0) {
      if(task_sleep_deadline(&ds->waitq, deadline)) {
        stm32_dma_stop(instance);
        ds->status = 0;
        return ERR_TIMEOUT;
      }
      continue;
    }

    uint32_t s = ds->status;
    ds->status = 0;
    if(s & DMA_STATUS_XFER_ERROR)
      return ERR_DMA_XFER;
    if(s & DMA_STATUS_FIFO_ERROR)
      return ERR_DMA_FIFO;
    return 0;
  }
}

static void __attribute__((noinline))
dma_irq(int instance, int bits)
{
  dma_stream_t *ds = g_streams[instance];
  if(ds == NULL)
    return;

  ds->cb(instance, bits, ds->arg);
}



#define GET_ISR(controller, hi, offset, instance)                      \
  const uint32_t base = DMA_BASE(controller);                          \
  const uint32_t bits = (reg_rd(base + DMA_ISR(hi)) >> offset) & 0x3f; \
  reg_wr(base + DMA_IFCR(hi), bits << offset);                         \
  dma_irq(instance, bits);                                             \

void irq_11(void) { GET_ISR(0, 0, 0,  0) }
void irq_12(void) { GET_ISR(0, 0, 6,  1) }
void irq_13(void) { GET_ISR(0, 0, 16, 2) }
void irq_14(void) { GET_ISR(0, 0, 22, 3) }
void irq_15(void) { GET_ISR(0, 1, 0,  4) }
void irq_16(void) { GET_ISR(0, 1, 6,  5) }
void irq_17(void) { GET_ISR(0, 1, 16, 6) }
void irq_47(void) { GET_ISR(0, 1, 22, 7) }
void irq_56(void) { GET_ISR(1, 0,  0, 8) }
void irq_57(void) { GET_ISR(1, 0,  6, 9) }
void irq_58(void) { GET_ISR(1, 0, 16,10) }
void irq_59(void) { GET_ISR(1, 0, 22,11) }
void irq_60(void) { GET_ISR(1, 1,  0,12) }
void irq_68(void) { GET_ISR(1, 1,  6,13) }
void irq_69(void) { GET_ISR(1, 1, 16,14) }
void irq_70(void) { GET_ISR(1, 1, 22,15) }
