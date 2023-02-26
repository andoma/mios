#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/task.h>
#include <mios/io.h>

#include "nrf52_reg.h"
#include "nrf52_uart.h"

#include "irq.h"

#define TX_FIFO_SIZE 128
#define RX_FIFO_SIZE 64

typedef struct nrf52_uart {

  stream_t stream;

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

} nrf52_uart_t;



static void
nrf52_uart_write(stream_t *s, const void *buf, size_t size)
{
  nrf52_uart_t *u = (nrf52_uart_t *)s;
  if(size == 0)
    return;

  const char *d = buf;

  const int busy_wait = !can_sleep();

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(busy_wait) {
    // We are in an interrupt or all interrupts disabled, we busy wait

    for(size_t i = 0; i < size; i++) {
      const uint8_t c = d[i];
      reg_wr(UART_TXD, c);
      reg_wr(UART_TX_TASK, 1);

      while(!reg_rd(UART_TX_RDY)) {
      }
      reg_wr(UART_TX_RDY, 0);
      reg_wr(UART_TX_TASK, 0);
    }
    irq_permit(q);
    return;
  }

  for(size_t i = 0; i < size; i++) {
    const uint8_t c = d[i];
    while(1) {
      uint8_t avail = TX_FIFO_SIZE - (u->tx_fifo_rdptr - u->tx_fifo_wrptr);
      if(avail == 0) {
        assert(u->tx_busy);
        task_sleep(&u->uart_tx);
        continue;
      }

      if(!u->tx_busy) {
        reg_wr(UART_TXD, c);
        reg_wr(UART_TX_TASK, 1);
        u->tx_busy = 1;
        break;
      }

      u->tx_fifo[u->tx_fifo_wrptr & (TX_FIFO_SIZE - 1)] = c;
      u->tx_fifo_wrptr++;
      break;
    }
  }
  irq_permit(q);
}




static void
nrf52_uart_irq(void *arg)
{
  nrf52_uart_t *u = arg;

  if(reg_rd(UART_RX_RDY)) {
    reg_wr(UART_RX_RDY, 0);
    char c = reg_rd(UART_RXD);

    if(c == 4) {
      panic("Halted from console");
    }
    u->rx_fifo[u->rx_fifo_wrptr & (RX_FIFO_SIZE - 1)] = c;
    u->rx_fifo_wrptr++;
    task_wakeup(&u->uart_rx, 1);
  }

  if(reg_rd(UART_TX_RDY)) {
    reg_wr(UART_TX_RDY, 0);

    uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
    if(avail == 0) {
      reg_wr(UART_TX_TASK, 0);
      u->tx_busy = 0;
    } else {
      char c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      u->tx_fifo_rdptr++;
      task_wakeup(&u->uart_tx, 1);
      reg_wr(UART_TXD, c);
    }
  }
}


static int
nrf52_uart_read(struct stream *s, void *buf, size_t size, int mode)
{
  nrf52_uart_t *u = (nrf52_uart_t *)s;

  uint8_t *d = buf;

  const int busy_wait = !can_sleep();

  if(busy_wait) {

    for(size_t i = 0; i < size; i++) {

      while(!reg_rd(UART_RX_RDY)) {
        if(stream_wait_is_done(mode, i, size)) {
          return i;
        }
      }
      reg_wr(UART_RX_RDY, 0);
      char c = reg_rd(UART_RXD);
      d[i] = c;
    }
    return size;
  }

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  for(size_t i = 0; i < size; i++) {
    while(u->rx_fifo_wrptr == u->rx_fifo_rdptr) {
      if(stream_wait_is_done(mode, i, size)) {
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

struct stream *
nrf52_uart_init(int baudrate, gpio_t txpin, gpio_t rxpin, int flags)
{
  nrf52_uart_t *u = calloc(1, sizeof(nrf52_uart_t));

  u->stream.read = nrf52_uart_read;
  u->stream.write = nrf52_uart_write;

  u->uart_flags = flags;
  reg_wr(UART_PSELTXD, txpin);
  reg_wr(UART_PSELRXD, rxpin);
  reg_wr(UART_ENABLE, 4);
  reg_wr(UART_BAUDRATE, 0x1d60000);

  reg_wr(UART_INTENSET, 0x84); // RXDRDY and TXDRDY
  reg_wr(UART_RX_TASK, 1);

  irq_enable_fn_arg(2, IRQ_LEVEL_CONSOLE, nrf52_uart_irq, u);

  return &u->stream;
}
