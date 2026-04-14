// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file
//
// DMA v3: HPDMA / GPDMA controller (STM32N6, STM32H5, etc.)
// Both controllers share identical register layouts, only differing
// in FIFO sizes, master port capabilities, and base addresses.

// Per-channel register offsets (from channel base at ctrl + 0x50 + ch*0x80)
// Note: there is a gap between the control group (0x00-0x14)
// and the transfer group (0x40-0x7C) within each channel block.
#define DMA_CxLBAR     0x00
#define DMA_CxCIDCFGR  0x04
#define DMA_CxSEMCR    0x08
#define DMA_CxFCR      0x0C
#define DMA_CxSR       0x10
#define DMA_CxCR       0x14
#define DMA_CxTR1      0x40
#define DMA_CxTR2      0x44
#define DMA_CxBR1      0x48
#define DMA_CxSAR      0x4C
#define DMA_CxDAR      0x50
#define DMA_CxLLR      0x7C

// Channel base: controller_base + 0x50 + channel * 0x80
#define DMA_CH_BASE(ctrl_base, ch) ((ctrl_base) + 0x50 + (ch) * 0x80)

// CxCR bits
#define DMA_CxCR_EN       (1 << 0)
#define DMA_CxCR_RESET    (1 << 1)
#define DMA_CxCR_SUSP     (1 << 2)
#define DMA_CxCR_TCIE     (1 << 8)
#define DMA_CxCR_HTIE     (1 << 9)
#define DMA_CxCR_DTEIE    (1 << 10)
#define DMA_CxCR_ULEIE    (1 << 11)
#define DMA_CxCR_USEIE    (1 << 12)
#define DMA_CxCR_SUSPIE   (1 << 13)
#define DMA_CxCR_TOIE     (1 << 14)

// CxSR bits
#define DMA_CxSR_IDLEF    (1 << 0)
#define DMA_CxSR_TCF      (1 << 8)
#define DMA_CxSR_HTF      (1 << 9)
#define DMA_CxSR_DTEF     (1 << 10)
#define DMA_CxSR_ULEF     (1 << 11)
#define DMA_CxSR_USEF     (1 << 12)
#define DMA_CxSR_SUSPF    (1 << 13)
#define DMA_CxSR_TOF      (1 << 14)

// CxFCR bits (write 1 to clear corresponding flag in CxSR)
#define DMA_CxFCR_TCF      (1 << 8)
#define DMA_CxFCR_HTF      (1 << 9)
#define DMA_CxFCR_DTEF     (1 << 10)
#define DMA_CxFCR_ULEF     (1 << 11)
#define DMA_CxFCR_USEF     (1 << 12)
#define DMA_CxFCR_SUSPF    (1 << 13)
#define DMA_CxFCR_TOF      (1 << 14)
#define DMA_CxFCR_ALL      0x7F00

// CxTR1 bits
#define DMA_CxTR1_SINC     (1 << 3)
#define DMA_CxTR1_SSEC     (1 << 15)
#define DMA_CxTR1_DINC     (1 << 19)
#define DMA_CxTR1_DSEC     (1u << 31)

// CxTR2 bits
#define DMA_CxTR2_SWREQ    (1 << 9)
#define DMA_CxTR2_DREQ     (1 << 10)


typedef struct dma_channel {

  dma_cb_t *cb;
  void *arg;

  task_waitable_t waitq;
  uint32_t status;

  uint32_t ch_base;   // Per-channel register base address
  uint32_t paddr;     // Peripheral address
  uint32_t maddr;     // Memory address
  uint32_t tr1;       // Cached CxTR1 value
  uint32_t tr2;       // Cached CxTR2 value (includes REQSEL)
  uint32_t cr;        // Cached CxCR value (priority + IE bits)
  uint16_t nitems;    // Byte count for BNDT
  uint8_t direction;  // 0 = P_TO_M, 1 = M_TO_P

} dma_channel_t;


static dma_channel_t *g_channels[DMA_NUM_CHANNELS];


static void
wakeup_cb(stm32_dma_instance_t instance, uint32_t status, void *arg)
{
  dma_channel_t *ch = arg;
  ch->status |= status;
  task_wakeup_sched_locked(&ch->waitq, 0);
}


static stm32_dma_instance_t
stm32_dma_alloc_instance_v3(uint32_t ctrl_base, uint16_t clkid,
                            int first_ch, int num_ch, int irq_base,
                            uint8_t reqsel, const char *name)
{
  int q = irq_forbid(IRQ_LEVEL_SWITCH);
  for(int i = first_ch; i < first_ch + num_ch; i++) {
    if(g_channels[i])
      continue;

    clk_enable(clkid);

    int hw_ch = i - first_ch;
    uint32_t ch_base = DMA_CH_BASE(ctrl_base, hw_ch);

    dma_channel_t *ch = calloc(1, sizeof(dma_channel_t));
    ch->ch_base = ch_base;
    ch->tr2 = reqsel;  // REQSEL in bits [7:0]
    g_channels[i] = ch;

    // Reset channel to known state
    reg_wr(ch_base + DMA_CxCR, DMA_CxCR_RESET);
    while(!(reg_rd(ch_base + DMA_CxSR) & DMA_CxSR_IDLEF)) {}

    // Configure channel as secure + privileged + CID 1
    // Required for RISAF to allow DMA access to AXISRAM
    reg_wr(ch_base + DMA_CxCIDCFGR, (1 << 4) | (1 << 0)); // SCID=1, CFEN=1
    reg_set_bit(ctrl_base + 0x00, hw_ch);  // SECCFGR: channel secure
    reg_set_bit(ctrl_base + 0x04, hw_ch);  // PRIVCFGR: channel privileged

    irq_permit(q);

    printf("%s: Using DMA ch %d (REQSEL %d)\n", name, i, reqsel);
    return i;
  }
  irq_permit(q);
  panic("Out of DMA channels");
}


void
stm32_dma_set_callback(stm32_dma_instance_t i, dma_cb_t *cb, void *arg,
                       int irq_level, uint32_t status_mask)
{
  dma_channel_t *ch = g_channels[i];
  ch->cb = cb;
  ch->arg = arg;

  // Map status_mask to CxCR interrupt enable bits
  uint32_t ie = 0;
  if(status_mask & DMA_STATUS_FULL_XFER)
    ie |= DMA_CxCR_TCIE;
  if(status_mask & DMA_STATUS_HALF_XFER)
    ie |= DMA_CxCR_HTIE;
  if(status_mask & (DMA_STATUS_XFER_ERROR | DMA_STATUS_FIFO_ERROR))
    ie |= DMA_CxCR_DTEIE | DMA_CxCR_USEIE;

  ch->cr = (ch->cr & ~0x7F00) | ie;

  irq_enable(dma_irq_base + i, irq_level);
}


void
stm32_dma_make_waitable(stm32_dma_instance_t i, const char *name)
{
  dma_channel_t *ch = g_channels[i];
  task_waitable_init(&ch->waitq, name);
  ch->status = 0;
  ch->cb = wakeup_cb;
  ch->arg = ch;

  ch->cr |= DMA_CxCR_TCIE | DMA_CxCR_DTEIE | DMA_CxCR_USEIE;

  irq_enable(dma_irq_base + i, IRQ_LEVEL_SCHED);
}


void
stm32_dma_set_paddr(stm32_dma_instance_t i, uint32_t paddr)
{
  g_channels[i]->paddr = paddr;
}


void
stm32_dma_set_mem0(stm32_dma_instance_t i, void *maddr)
{
  assert(maddr != NULL);
  g_channels[i]->maddr = (uint32_t)maddr;
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
  // Pack all config into a single uint32_t for stm32_dma_config_set_reg.
  // We use a private encoding since v3 registers are split across
  // CxTR1, CxTR2, and CxCR — the set_reg function unpacks it.
  //
  // Bits [1:0]   = direction
  // Bits [3:2]   = psize (SDW when P is source, DDW when P is dest)
  // Bits [5:4]   = msize (DDW when P is source, SDW when P is dest)
  // Bit  [6]     = pinc (SINC when P is source, DINC when P is dest)
  // Bit  [7]     = minc (DINC when P is source, SINC when P is dest)
  // Bits [9:8]   = prio
  // Bit  [10]    = circular (not natively supported in v3 direct mode)
  // Bits [13:12] = pburst (SBL_1 when P is source)
  // Bits [15:14] = mburst (DBL_1 when P is source)

  uint32_t reg = 0;
  reg |= direction;
  reg |= psize  << 2;
  reg |= msize  << 4;
  reg |= pinc   << 6;
  reg |= minc   << 7;
  reg |= prio   << 8;
  reg |= circular << 10;
  reg |= pburst << 12;
  reg |= mburst << 14;
  return reg;
}


void
stm32_dma_config_set_reg(stm32_dma_instance_t i, uint32_t reg)
{
  dma_channel_t *ch = g_channels[i];

  const int direction = reg & 0x3;
  const int psize     = (reg >> 2) & 0x3;
  const int msize     = (reg >> 4) & 0x3;
  const int pinc      = (reg >> 6) & 0x1;
  const int minc      = (reg >> 7) & 0x1;
  const int prio      = (reg >> 8) & 0x3;

  ch->direction = direction;

  // Build CxTR1: source and dest data widths + increment + port selection
  // For P2M: source = peripheral, dest = memory
  // For M2P: source = memory, dest = peripheral
  //
  // SAP (bit 14): source allocated port  (0=AXI, 1=AHB)
  // DAP (bit 30): destination allocated port (0=AXI, 1=AHB)
  // APB peripherals are only reachable via AHB port.
  // All DMA bus transactions must be secure (SSEC + DSEC)
  // to pass through the RISAF firewalls on STM32N6
  uint32_t tr1 = DMA_CxTR1_SSEC | DMA_CxTR1_DSEC;
  if(direction == STM32_DMA_P_TO_M) {
    tr1 |= psize;                    // SDW[1:0] = peripheral size
    tr1 |= (pinc ? DMA_CxTR1_SINC : 0);   // SINC = periph increment
    tr1 |= msize << 16;              // DDW[17:16] = memory size
    tr1 |= (minc ? DMA_CxTR1_DINC : 0);   // DINC = mem increment
    tr1 |= (1 << 14);                // SAP=1: source (periph) via AHB
  } else {
    tr1 |= msize;                    // SDW[1:0] = memory size
    tr1 |= (minc ? DMA_CxTR1_SINC : 0);   // SINC = mem increment
    tr1 |= psize << 16;              // DDW[17:16] = peripheral size
    tr1 |= (pinc ? DMA_CxTR1_DINC : 0);   // DINC = periph increment
    tr1 |= (1 << 30);                // DAP=1: dest (periph) via AHB
  }
  ch->tr1 = tr1;

  // Build CxTR2: keep existing REQSEL, set DREQ/SWREQ for direction
  uint32_t tr2 = ch->tr2 & 0xff;  // Preserve REQSEL[7:0]
  if(pinc && minc) {
    // Both sides increment = memory-to-memory, use software request
    tr2 |= DMA_CxTR2_SWREQ;
  } else if(direction == STM32_DMA_M_TO_P) {
    // DREQ=1: hardware request from destination peripheral (SPI TX etc)
    tr2 |= DMA_CxTR2_DREQ;
  }
  // else P_TO_M: DREQ=0 (hardware request from source peripheral)
  ch->tr2 = tr2;

  // Priority goes in CxCR
  ch->cr = (ch->cr & ~(0x3 << 22)) | (prio << 22);
}


void
stm32_dma_set_nitems(stm32_dma_instance_t i, int nitems)
{
  g_channels[i]->nitems = nitems;
}


void
stm32_dma_start(stm32_dma_instance_t i)
{
  dma_channel_t *ch = g_channels[i];
  const uint32_t base = ch->ch_base;

  // Clear all pending flags
  reg_wr(base + DMA_CxFCR, DMA_CxFCR_ALL);

  // Program transfer registers
  reg_wr(base + DMA_CxTR1, ch->tr1);
  reg_wr(base + DMA_CxTR2, ch->tr2);
  reg_wr(base + DMA_CxBR1, ch->nitems);

  // Set source and destination based on direction
  if(ch->direction == STM32_DMA_M_TO_P) {
    reg_wr(base + DMA_CxSAR, ch->maddr);
    reg_wr(base + DMA_CxDAR, ch->paddr);
  } else {
    reg_wr(base + DMA_CxSAR, ch->paddr);
    reg_wr(base + DMA_CxDAR, ch->maddr);
  }

  // No linked-list
  reg_wr(base + DMA_CxLLR, 0);

  // Enable channel
  reg_wr(base + DMA_CxCR, ch->cr | DMA_CxCR_EN);
}


void
stm32_dma_stop(stm32_dma_instance_t i)
{
  dma_channel_t *ch = g_channels[i];
  const uint32_t base = ch->ch_base;
  const uint32_t sr = reg_rd(base + DMA_CxSR);

  if(sr & DMA_CxSR_IDLEF) {
    // Already idle, just clear flags
    reg_wr(base + DMA_CxFCR, DMA_CxFCR_ALL);
    return;
  }

  // Channel is active — issue reset
  reg_wr(base + DMA_CxCR, DMA_CxCR_RESET);

  // Wait for channel to become idle
  while(!(reg_rd(base + DMA_CxSR) & DMA_CxSR_IDLEF)) {}

  reg_wr(base + DMA_CxFCR, DMA_CxFCR_ALL);
}


error_t
stm32_dma_wait(stm32_dma_instance_t i)
{
  const int64_t deadline = clock_get_irq_blocked() + 1000000;
  dma_channel_t *ch = g_channels[i];

  while(1) {
    if(ch->status & DMA_STATUS_XFER_ERROR) {
      ch->status = 0;
      stm32_dma_stop(i);
      return ERR_DMA_XFER;
    }
    if(ch->status & DMA_STATUS_FULL_XFER) {
      ch->status = 0;
      return 0;
    }
    if(task_sleep_deadline(&ch->waitq, deadline)) {
      stm32_dma_stop(i);
      ch->status = 0;
      return ERR_TIMEOUT;
    }
  }
}


static void __attribute__((noinline))
dma_v3_irq(int instance)
{
  dma_channel_t *ch = g_channels[instance];
  if(ch == NULL)
    return;

  const uint32_t sr = reg_rd(ch->ch_base + DMA_CxSR);

  // Clear all set flags
  reg_wr(ch->ch_base + DMA_CxFCR, sr & DMA_CxFCR_ALL);

  if(ch->cb == NULL)
    return;

  // Map v3 status bits to generic DMA status flags
  uint32_t status = 0;
  if(sr & DMA_CxSR_TCF)
    status |= DMA_STATUS_FULL_XFER;
  if(sr & DMA_CxSR_HTF)
    status |= DMA_STATUS_HALF_XFER;
  if(sr & (DMA_CxSR_DTEF | DMA_CxSR_ULEF))
    status |= DMA_STATUS_XFER_ERROR;
  if(sr & DMA_CxSR_USEF)
    status |= DMA_STATUS_XFER_ERROR;

  ch->cb(instance, status, ch->arg);
}
