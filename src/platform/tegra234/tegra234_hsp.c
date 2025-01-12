// Hardware Synchronization Primitives, common primitives

// This file is included from platform specific HSP file (which
// is responsible for setting up interrupt handlers)

#include <mios/stream.h>
#include <mios/fifo.h>
#include <mios/mios.h>
#include <mios/task.h>

#include <unistd.h>
#include <stdlib.h>

#include "irq.h"

typedef struct hsp {
  handler_t irqs[16];
  uint8_t irq_route;
} hsp_t;


static hsp_t g_aon_hsp;
static hsp_t g_top0_hsp;

typedef struct hsp_stream {
  stream_t st;

  task_waitable_t tx_waitq;
  task_waitable_t rx_waitq;

  uint32_t tx_hsp_base;
  uint32_t rx_hsp_base;

  uint32_t tx_drops;
  uint32_t rx_overruns;

  uint8_t tx_mbox;
  uint8_t rx_mbox;
  uint8_t pending_clear;
  uint8_t tx_irq_route;
  uint8_t rx_irq_route;

  FIFO_DECL(rx_fifo, 128);

} hsp_stream_t;


static void
hsp_stream_rx_irq(void *arg)
{
  hsp_stream_t *hs = arg;
  uint32_t reg = hs->rx_hsp_base + 0x10000 + hs->rx_mbox * 0x8000;

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
    reg_clr_bit(hs->rx_hsp_base + 0x100 + hs->rx_irq_route * 4,
                8 + hs->rx_mbox);
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
    reg_wr(hs->rx_hsp_base + 0x10000 + hs->rx_mbox * 0x8000, 0);
    hs->pending_clear = 0;
    reg_set_bit(hs->rx_hsp_base + 0x100 + hs->rx_irq_route * 4,
                8 + hs->rx_mbox);
  }

  irq_permit(q);
  return written;
}


static void
hsp_stream_tx_irq(void *arg)
{
  hsp_stream_t *hs = arg;
  reg_clr_bit(hs->tx_hsp_base + 0x100 + hs->tx_irq_route * 4, hs->tx_mbox);
  task_wakeup(&hs->tx_waitq, 1);
}


static ssize_t
hsp_stream_write(struct stream *s, const void *buf, size_t size, int flags)
{
  hsp_stream_t *hs = (hsp_stream_t *)s;

  if(hs->tx_hsp_base == 0)
    return size;

  const uint8_t *u8 = buf;
  uint32_t reg = hs->tx_hsp_base + 0x10000 + hs->tx_mbox * 0x8000;

  ssize_t count = 0;

  uint64_t deadline = clock_get() + 10000;

  int q = irq_forbid(IRQ_LEVEL_IO);

  while(count < size) {

    if(reg_rd(reg) & (1 << 31)) {
      reg_set_bit(hs->tx_hsp_base + 0x100 + hs->tx_irq_route * 4,
                  hs->tx_mbox);
      if(task_sleep_deadline(&hs->tx_waitq, deadline)) {
        hs->tx_drops++;
        reg_clr_bit(hs->tx_hsp_base + 0x100 + hs->tx_irq_route * 4,
                    hs->tx_mbox);
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


static uint8_t
hsp_connect_mbox(uint32_t addr, void (*fn)(void *arg), void *arg,
                 uint8_t mbox)
{
  hsp_t *hsp;
  switch(addr) {
  case 0:
    return 0;
  case NV_ADDRESS_MAP_AON_HSP_BASE:
    hsp = &g_aon_hsp;
    break;
  case NV_ADDRESS_MAP_TOP0_HSP_BASE:
    hsp = &g_top0_hsp;
    break;
  default:
    panic("Unsupported HSP address 0x%x", addr);
  }

  handler_t *irq = &hsp->irqs[mbox];
  irq->arg = arg;
  irq->fn = fn;
  return hsp->irq_route;
}


stream_t *
hsp_mbox_stream(uint32_t rx_hsp_base, uint32_t rx_mbox,
                uint32_t tx_hsp_base, uint32_t tx_mbox)
{
  hsp_stream_t *hs = calloc(1, sizeof(hsp_stream_t));

  int q = irq_forbid(IRQ_LEVEL_IO);

  hs->rx_hsp_base = rx_hsp_base;
  hs->rx_mbox = rx_mbox;
  hs->rx_irq_route =
    hsp_connect_mbox(rx_hsp_base, hsp_stream_rx_irq, hs, 8 + rx_mbox);

  hs->tx_hsp_base = tx_hsp_base;
  hs->tx_mbox = tx_mbox;
  hs->tx_irq_route =
    hsp_connect_mbox(tx_hsp_base, hsp_stream_tx_irq, hs, tx_mbox);

  hs->st.vtable = &hsp_stream_vtable;

  reg_set_bit(hs->rx_hsp_base + 0x100 + hs->rx_irq_route * 4, 8 + rx_mbox);

  irq_permit(q);
  return &hs->st;
}


static void
hsp_dispatch_irq(hsp_t *hsp, uint32_t base_addr, uint32_t enabled)
{
  uint32_t pending = reg_rd(base_addr + 0x304) & 0xffff & enabled;
  if(pending) {
    const handler_t *irq = &hsp->irqs[31 - __builtin_clz(pending)];
    irq->fn(irq->arg);
  }
}


static void
hsp_aon_irq(void *arg)
{
  hsp_t *hsp = arg;
  hsp_dispatch_irq(hsp, NV_ADDRESS_MAP_AON_HSP_BASE,
                   reg_rd(NV_ADDRESS_MAP_AON_HSP_BASE + 0x100 +
                          hsp->irq_route * 4));
}


static void
hsp_top0_irq(void *arg)
{
  hsp_t *hsp = arg;
  hsp_dispatch_irq(hsp, NV_ADDRESS_MAP_TOP0_HSP_BASE,
                   reg_rd(NV_ADDRESS_MAP_TOP0_HSP_BASE + 0x100 +
                          hsp->irq_route * 4));
}
