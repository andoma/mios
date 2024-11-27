#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/task.h>
#include <mios/io.h>
#include <mios/mios.h>

#include "pl011.h"

#include "irq.h"
#include "reg.h"


#define TX_FIFO_SIZE 128
#define RX_FIFO_SIZE 64

typedef struct pl011 {

  stream_t stream;

  uint32_t base;

  uint8_t rx_fifo_rdptr;
  uint8_t rx_fifo_wrptr;
  uint8_t tx_fifo_rdptr;
  uint8_t tx_fifo_wrptr;

  task_waitable_t uart_rx;
  task_waitable_t uart_tx;

  uint8_t tx_fifo[TX_FIFO_SIZE];
  uint8_t rx_fifo[RX_FIFO_SIZE];

  uint8_t tx_busy;
  uint8_t uart_flags;

} pl011_t;


#define UART_DR    0x000
#define UART_FR    0x018
#define UART_IMSC  0x038

static void
printchar(pl011_t *u, char c)
{
  while(reg_rd(u->base + UART_FR) & (1 << 5)) {
  }
  reg_wr(u->base + UART_DR, c);
}


static ssize_t
pl011_uart_write(stream_t *s, const void *buf, size_t size, int flags)
{
  pl011_t *u = (pl011_t *)s;
  if(size == 0)
    return 0;

  const char *d = buf;

  const int busy_wait = !can_sleep();

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(busy_wait) {
    // We are in an interrupt or all interrupts disabled, we busy wait

    // First drain FIFO
    while(1) {
      uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
      if(!avail)
        break;
      char c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      u->tx_fifo_rdptr++;
      printchar(u, c);
    }
    for(size_t i = 0; i < size; i++) {
      const uint8_t c = d[i];
      printchar(u, c);
    }
    irq_permit(q);
    return size;
  }

  size_t written = 0;

  for(size_t i = 0; i < size; i++) {
    const uint8_t c = d[i];
    while(1) {
      uint8_t avail = TX_FIFO_SIZE - (u->tx_fifo_rdptr - u->tx_fifo_wrptr);
      if(avail == 0) {
        if(flags & STREAM_WRITE_NO_WAIT)
          break;
        assert(u->tx_busy);
        task_sleep(&u->uart_tx);
        continue;
      }

      written++;
      if(!u->tx_busy) {
        reg_wr(u->base + UART_DR, c);
        reg_wr(u->base + UART_IMSC, (1 << 4) | (1 << 5));
        u->tx_busy = 1;
        break;
      }

      u->tx_fifo[u->tx_fifo_wrptr & (TX_FIFO_SIZE - 1)] = c;
      u->tx_fifo_wrptr++;
    }
  }
  irq_permit(q);
  return written;
}




static void
pl011_uart_irq(void *arg)
{
  pl011_t *u = arg;

  uint32_t fr = reg_rd(u->base + UART_FR);

  if(!(fr & (1 << 4))) {
    char c = reg_rd(u->base + UART_DR);

    if(c == 4) {
      panic("Halted from console");
    }

    u->rx_fifo[u->rx_fifo_wrptr & (RX_FIFO_SIZE - 1)] = c;
    u->rx_fifo_wrptr++;
    task_wakeup(&u->uart_rx, 1);
  }

  if(!(fr & (1 << 5))) {
    uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
    if(avail == 0) {
      u->tx_busy = 0;
      reg_wr(u->base + UART_IMSC, (1 << 4));
    } else {
      char c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      u->tx_fifo_rdptr++;
      task_wakeup(&u->uart_tx, 1);
      reg_wr(u->base + UART_DR, c);
    }
  }
}


static ssize_t
pl011_uart_read(struct stream *s, void *buf, size_t size, size_t minbytes)
{
  pl011_t *u = (pl011_t *)s;

  uint8_t *d = buf;

  const int busy_wait = !can_sleep();

  if(busy_wait) {

    for(size_t i = 0; i < size; i++) {

      while(reg_rd(u->base + UART_FR) & (1 << 4)) {
        if(i >= minbytes) {
          return i;
        }
      }
      char c = reg_rd(u->base + UART_DR);
      d[i] = c;
    }
    return size;
  }

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  for(size_t i = 0; i < size; i++) {
    while(u->rx_fifo_wrptr == u->rx_fifo_rdptr) {
      if(i >= minbytes) {
        irq_permit(q);
        return i;
      }
      task_sleep(&u->uart_rx);
    }

    d[i] = u->rx_fifo[u->rx_fifo_rdptr & (RX_FIFO_SIZE - 1)];
    u->rx_fifo_rdptr++;
  }
  irq_permit(q);
  return size;
}


static const stream_vtable_t pl011_uart_vtable = {
  .read = pl011_uart_read,
  .write = pl011_uart_write
};

struct stream *
pl011_uart_init(uint32_t base, int baudrate, int irq)
{
  pl011_t *u = calloc(1, sizeof(pl011_t));

  u->base = base;

  u->stream.vtable = &pl011_uart_vtable;

  reg_wr(base + UART_IMSC, (1 << 4)); // RX interrupt

  irq_enable_fn_arg(irq, IRQ_LEVEL_CONSOLE, pl011_uart_irq, u);

  return &u->stream;
}
