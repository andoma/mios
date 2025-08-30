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
  handler_t handlers[26];
  uint8_t irq_route;
} hsp_t;


static hsp_t g_aon_hsp;
static hsp_t g_top0_hsp;
static hsp_t g_top1_hsp;

typedef struct hsp_stream {
  stream_t st;

  task_waitable_t tx_waitq;
  task_waitable_t rx_waitq;

  uint32_t tx_hsp_base;
  uint32_t rx_hsp_base;

  uint32_t tx_drops;
  uint32_t rx_overruns;

  uint32_t tx_ie;
  uint32_t rx_ie;

  uint8_t tx_mbox;
  uint8_t rx_mbox;
  uint8_t pending_clear;

  FIFO_DECL(rx_fifo, uint8_t, 128);

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
    reg_clr_bit(hs->rx_ie, HSP_IRQ_MBOX_FULL(hs->rx_mbox));
  }
  task_wakeup(&hs->rx_waitq, 1);
}


static ssize_t
hsp_stream_read(struct stream *s, void *buf, size_t size, size_t required)
{
  hsp_stream_t *hs = (hsp_stream_t *)s;
  uint8_t *u8 = buf;
  size_t written = 0;

  if(!can_sleep()) {
    const uint32_t reg = hs->rx_hsp_base + 0x10000 + hs->rx_mbox * 0x8000;

    while(written < size) {

      uint32_t val = reg_rd(reg);

      if(!(val & (1 << 31))) {
        if(written >= required)
          break;
        continue;
      }

      //      int bytes = (val >> 24) & 3;
      // XXX: Handle # bytes > 1
      *u8++ = val;
      reg_wr(reg, 0);
      written++;
    }
    return written;
  }

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
    reg_set_bit(hs->rx_ie, HSP_IRQ_MBOX_FULL(hs->rx_mbox));
  }

  irq_permit(q);
  return written;
}


static void
hsp_stream_tx_irq(void *arg)
{
  hsp_stream_t *hs = arg;
  reg_clr_bit(hs->tx_ie, hs->tx_mbox);
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

      if(!can_sleep())
        continue;

      reg_set_bit(hs->tx_ie, hs->tx_mbox);
      if(task_sleep_deadline(&hs->tx_waitq, deadline)) {
        hs->tx_drops++;
        reg_clr_bit(hs->tx_ie, hs->tx_mbox);
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

static const stream_vtable_t hsp_stream_vtable_txonly = {
  .write = hsp_stream_write,
};


uint32_t
hsp_connect_irq(uint32_t addr, void (*fn)(void *arg), void *arg, uint32_t irq)
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
  case NV_ADDRESS_MAP_TOP1_HSP_BASE:
    hsp = &g_top1_hsp;
    break;
  default:
    panic("Unsupported HSP address 0x%x", addr);
  }

  handler_t *h = &hsp->handlers[irq];
  h->arg = arg;
  h->fn = fn;

  return addr + 0x100 + hsp->irq_route * 4;
}


stream_t *
hsp_mbox_stream(uint32_t rx_hsp_base, uint32_t rx_mbox,
                uint32_t tx_hsp_base, uint32_t tx_mbox)
{
  hsp_stream_t *hs = calloc(1, sizeof(hsp_stream_t));

  int q = irq_forbid(IRQ_LEVEL_IO);

  hs->rx_hsp_base = rx_hsp_base;
  hs->rx_mbox = rx_mbox;
  hs->rx_ie = hsp_connect_irq(rx_hsp_base, hsp_stream_rx_irq, hs,
                               HSP_IRQ_MBOX_FULL(rx_mbox));

  hs->tx_hsp_base = tx_hsp_base;
  hs->tx_mbox = tx_mbox;
  hs->tx_ie = hsp_connect_irq(tx_hsp_base, hsp_stream_tx_irq, hs, tx_mbox);

  if(rx_hsp_base) {
    reg_set_bit(hs->rx_ie, HSP_IRQ_MBOX_FULL(rx_mbox));
    hs->st.vtable = &hsp_stream_vtable;
  } else {
    hs->st.vtable = &hsp_stream_vtable_txonly;
  }

  irq_permit(q);
  return &hs->st;
}


static void
hsp_dispatch_irq(hsp_t *hsp, uint32_t base_addr, uint32_t enabled)
{
  uint32_t active = reg_rd(base_addr + 0x304);
  uint32_t pending = active & 0x3ffffff & enabled;
  if(pending) {
    int id = 31 - __builtin_clz(pending);
    if(id < ARRAYSIZE(hsp->handlers)) {
      const handler_t *irq = &hsp->handlers[id];
      if(irq->fn) {
        irq->fn(irq->arg);
        return;
      }
    }
    panic("HSP 0x%x stray IRQ %d enabled:0x%x", base_addr, id, enabled);
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


__attribute__((unused))
static void
hsp_top1_irq(void *arg)
{
  hsp_t *hsp = arg;
  hsp_dispatch_irq(hsp, NV_ADDRESS_MAP_TOP1_HSP_BASE,
                   reg_rd(NV_ADDRESS_MAP_TOP1_HSP_BASE + 0x100 +
                          hsp->irq_route * 4));
}
