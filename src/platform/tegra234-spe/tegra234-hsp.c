// Hardware Synchronization Primitives

#include "tegra234-hsp.h"

#include <stdlib.h>

#include <mios/stream.h>
#include <mios/fifo.h>
#include <mios/mios.h>

#include <stdio.h>
#include <unistd.h>

#include "irq.h"
#include "reg.h"


typedef struct hsp_irq {
  void (*fn)(void *arg);
  void *arg;
} hsp_irq_t;

typedef struct hsp {
  hsp_irq_t irqs[16];
} hsp_t;


static hsp_t g_aon_hsp;
static hsp_t g_top0_hsp;

static void
hsp_dispatch_irq(hsp_t *hsp, uint32_t base_addr, uint32_t enabled)
{
  uint32_t pending = reg_rd(base_addr + 0x304) & 0xffff & enabled;
  if(pending) {
    const hsp_irq_t *irq = &hsp->irqs[31 - __builtin_clz(pending)];
    irq->fn(irq->arg);
  }
}


static void
hsp_aon_irq(void *arg)
{
  hsp_dispatch_irq(arg, NV_ADDRESS_MAP_AON_HSP_BASE,
                   reg_rd(NV_ADDRESS_MAP_AON_HSP_BASE + 0x100));
}


static void
hsp_top0_irq(void *arg)
{
  hsp_dispatch_irq(arg, NV_ADDRESS_MAP_TOP0_HSP_BASE,
                   reg_rd(NV_ADDRESS_MAP_TOP0_HSP_BASE + 0x104));
}


typedef struct hsp_stream {
  stream_t st;

  uint8_t tx_mbox;
  uint8_t rx_mbox;
  uint8_t pending_clear;

  uint32_t tx_hsp_address;

  task_waitable_t tx_waitq;

  task_waitable_t rx_waitq;
  uint32_t rx_overruns;
  FIFO_DECL(rx_fifo, 128);

} hsp_stream_t;


static void
hsp_stream_tx_irq(void *arg)
{
  hsp_stream_t *hs = arg;
  reg_clr_bit(hs->tx_hsp_address + 0x104, hs->tx_mbox);
  task_wakeup(&hs->tx_waitq, 1);
}


static void
hsp_stream_rx_irq(void *arg)
{
  hsp_stream_t *hs = arg;

  uint32_t reg = NV_ADDRESS_MAP_AON_HSP_BASE + 0x10000 + hs->rx_mbox * 0x8000;

  uint32_t val = reg_rd(reg);
  int bytes = (val >> 24) & 3;
  for(int i = 0; i < bytes; i++) {
    fifo_wr(&hs->rx_fifo, (val >> (i * 8)) & 0xff);
  }

  if(fifo_avail(&hs->rx_fifo) > 3) {
    reg_wr(reg, 0);
  } else {
    // FIFO almost full, disable interrupts until FIFO has cleared
    hs->pending_clear = 1;
    reg_clr_bit(NV_ADDRESS_MAP_AON_HSP_BASE + 0x100, 8 + hs->rx_mbox);
  }
  task_wakeup(&hs->rx_waitq, 1);
}


static ssize_t
hsp_stream_read(struct stream *s, void *buf, size_t size, size_t required)
{
  hsp_stream_t *hs = (hsp_stream_t *)s;
  uint8_t *u8 = buf;
  size_t written = 0;
  int q = irq_forbid(IRQ_LEVEL_IO);

  while(written < size) {

    if(fifo_is_empty(&hs->rx_fifo)) {
      if(written >= required)
        break;
      task_sleep(&hs->rx_waitq);
      continue;
    }
    *u8++ = fifo_rd(&hs->rx_fifo);
    written++;
  }

  if(hs->pending_clear && fifo_avail(&hs->rx_fifo) > 3) {
    reg_wr(NV_ADDRESS_MAP_AON_HSP_BASE + 0x10000 + hs->rx_mbox * 0x8000, 0);
    hs->pending_clear = 0;
    reg_set_bit(NV_ADDRESS_MAP_AON_HSP_BASE + 0x100, 8 + hs->rx_mbox);
  }

  irq_permit(q);
  return written;
}


static ssize_t
hsp_stream_write(struct stream *s, const void *buf, size_t size, int flags)
{
  const uint8_t *u8 = buf;
  hsp_stream_t *hs = (hsp_stream_t *)s;
  uint32_t reg = hs->tx_hsp_address + 0x10000 + hs->tx_mbox * 0x8000;

  ssize_t count = 0;

  uint64_t deadline = clock_get() + 10000;

  int q = irq_forbid(IRQ_LEVEL_IO);

  while(count < size) {

    if(reg_rd(reg) & (1 << 31)) {
      reg_set_bit(hs->tx_hsp_address + 0x104, hs->tx_mbox);
      if(task_sleep_deadline(&hs->tx_waitq, deadline)) {
        reg_clr_bit(hs->tx_hsp_address + 0x104, hs->tx_mbox);
        break;
      }
      continue;
    }
    // TODO: Write up to three bytes if we have 'em
    reg_wr(reg, (1 << 31) | (1 << 24) | u8[count]);
    count++;
  }
  irq_permit(q);
  return size;
}

static task_waitable_t *
hsp_stream_poll(stream_t *s, poll_type_t type)
{
  hsp_stream_t *hs = (hsp_stream_t *)s;

  irq_forbid(IRQ_LEVEL_IO);

  if(type == POLL_STREAM_WRITE) {
    return NULL;

  } else {

    if(!fifo_is_empty(&hs->rx_fifo))
      return NULL;
    return &hs->rx_waitq;
  }
}


static const stream_vtable_t hsp_stream_vtable = {
  .read = hsp_stream_read,
  .write = hsp_stream_write,
  .poll = hsp_stream_poll,
};


stream_t *
hsp_mbox_stream(uint32_t rx_mbox, uint32_t tx_hsp_address, uint32_t tx_mbox)
{
  hsp_stream_t *hs = calloc(1, sizeof(hsp_stream_t));
  hs->rx_mbox = rx_mbox;
  hs->tx_hsp_address = tx_hsp_address;
  hs->tx_mbox = tx_mbox;
  hs->st.vtable = &hsp_stream_vtable;

  int q = irq_forbid(IRQ_LEVEL_IO);


  if(tx_hsp_address == NV_ADDRESS_MAP_TOP0_HSP_BASE) {
    hsp_t *remote_hsp = &g_top0_hsp;
    hsp_irq_t *irq = &remote_hsp->irqs[tx_mbox];
    irq->arg = hs;
    irq->fn = hsp_stream_tx_irq;
  }

  hsp_t *local_hsp = &g_aon_hsp;
  hsp_irq_t *irq = &local_hsp->irqs[8 + rx_mbox];
  irq->arg = hs;
  irq->fn = hsp_stream_rx_irq;

  reg_set_bit(NV_ADDRESS_MAP_AON_HSP_BASE + 0x100, 8 + rx_mbox);

  irq_permit(q);
  return &hs->st;
}


static void  __attribute__((constructor(200)))
tegra234_hsp_init(void)
{
  irq_enable_fn_arg(6, IRQ_LEVEL_IO, hsp_aon_irq, &g_aon_hsp);
  irq_enable_fn_arg(LIC_IRQ(121), IRQ_LEVEL_IO, hsp_top0_irq, &g_top0_hsp);
}
