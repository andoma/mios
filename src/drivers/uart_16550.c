#include <stdio.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>

#include <mios/fifo.h>
#include <mios/mios.h>

#include "cpu.h"
#include "irq.h"

#define UART_THR 0x0
#define UART_IER 0x4
#define UART_IIR 0x8
#define UART_LSR 0x14

#define UART_LSR_THRE         (1 << 5)
#define UART_LSR_RXFIFO_EMPTY (1 << 9)

typedef struct uart_16550 {
  stream_t st;
  uint32_t reg_base;

  task_waitable_t rx_waitq;
  task_waitable_t tx_waitq;

  uint32_t rx_overruns;

  FIFO_DECL(rx_fifo, uint8_t, 128);

} uart_16550_t;


static ssize_t
uart_16550_write(struct stream *s, const void *buf, size_t size, int flags)
{
  uart_16550_t *u = (uart_16550_t *)s;
  const uint8_t *b = buf;

  ssize_t written = 0;

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  for(size_t i = 0; i < size; i++) {

    while(!(reg_rd(u->reg_base + UART_LSR) & UART_LSR_THRE)) {
      if(flags & STREAM_WRITE_NO_WAIT)
        goto done;
      if(!can_sleep())
        continue;
      reg_wr(u->reg_base + UART_IER, 0x3);
      task_sleep(&u->tx_waitq);
    }
    reg_wr(u->reg_base + UART_THR, b[i]);
    written++;
  }
 done:
  irq_permit(q);
  return written;
}


static ssize_t
uart_16550_read(struct stream *s, void *buf, size_t size, size_t required)
{
  uart_16550_t *u = (uart_16550_t *)s;
  size_t written = 0;
  uint8_t *u8 = buf;

  if(!can_sleep()) {

    while(written < size) {

      while(reg_rd(u->reg_base + UART_LSR) & UART_LSR_RXFIFO_EMPTY) {
        if(written >= required)
          return written;
      }
      *u8++ = reg_rd(u->reg_base + UART_THR);
      written++;
    }
    return written;
  }

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  while(written < size) {

    if(fifo_is_empty(&u->rx_fifo)) {
      if(written >= required)
        return written;
      task_sleep(&u->rx_waitq);
    }
    *u8++ = fifo_rd(&u->rx_fifo);
    written++;
  }
  irq_permit(q);

  return written;
}


static void
uart_16550_irq(void *arg)
{
  uart_16550_t *u = arg;
  uint32_t iir;

  while((iir = reg_rd(u->reg_base + UART_IIR) & 0xf) != 1) {

    switch(iir) {
    case 2:
      reg_wr(u->reg_base + UART_IER, 0x1);
      task_wakeup(&u->tx_waitq, 1);
      continue;

    case 4:
      while(!(reg_rd(u->reg_base + UART_LSR) & UART_LSR_RXFIFO_EMPTY)) {
        uint8_t b = reg_rd(u->reg_base + UART_THR);
        if(fifo_avail(&u->rx_fifo)) {
          fifo_wr(&u->rx_fifo, b);
        } else {
          u->rx_overruns++;
        }
      }
      task_wakeup(&u->rx_waitq, 1);
      continue;

    default:
      panic("uart unexpected iir:0x%x", iir);
    }
  }
}


static const stream_vtable_t uart_16550_vtable = {
  .write = uart_16550_write,
  .read = uart_16550_read,
};


stream_t *
uart_16550_create(uint32_t base_addr, int irq)
{
  uart_16550_t *u = calloc(1, sizeof(uart_16550_t));

  u->st.vtable = &uart_16550_vtable;
  u->reg_base = base_addr;

  task_waitable_init(&u->rx_waitq, "uartrx");
  task_waitable_init(&u->tx_waitq, "uarttx");

  reg_wr(u->reg_base + UART_IER, 0x1);
  irq_enable_fn_arg(irq, IRQ_LEVEL_CONSOLE, uart_16550_irq, u);
  return &u->st;
}
