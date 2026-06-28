#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/task.h>
#include <mios/io.h>

#include "nrf54l_reg.h"
#include "nrf54l_uart.h"

#include "irq.h"

// UARTE register offsets (EasyDMA variant, nRF54L)
#define UARTE_TASK_STARTRX   0x028 // TASKS_DMA.RX.START
#define UARTE_TASK_STARTTX   0x050 // TASKS_DMA.TX.START
#define UARTE_EVENT_ENDRX    0x14c // EVENTS_DMA.RX.END
#define UARTE_EVENT_ENDTX    0x168 // EVENTS_DMA.TX.END
#define UARTE_INTENSET       0x304
#define UARTE_ENABLE         0x500
#define UARTE_BAUDRATE       0x524
#define UARTE_PSEL_TXD       0x604
#define UARTE_PSEL_RXD       0x60c
#define UARTE_DMA_RX_PTR     0x704
#define UARTE_DMA_RX_MAXCNT  0x708
#define UARTE_DMA_TX_PTR     0x73c
#define UARTE_DMA_TX_MAXCNT  0x740

#define UARTE_ENABLE_ENABLED 8
#define UARTE_INTEN_ENDRX    (1 << 19) // DMARXEND
#define UARTE_INTEN_ENDTX    (1 << 26) // DMATXEND

#define TX_FIFO_SIZE 128
#define RX_FIFO_SIZE 64

typedef struct nrf54l_uart {

  stream_t stream;

  uint32_t base;

  task_waitable_t uart_rx;
  task_waitable_t uart_tx;

  uint8_t rx_fifo_rdptr;
  uint8_t rx_fifo_wrptr;
  uint8_t tx_fifo_rdptr;
  uint8_t tx_fifo_wrptr;

  uint8_t tx_busy;
  uint8_t uart_flags;

  // EasyDMA can only access RAM, so these bounce buffers live in RAM
  uint8_t tx_dma;
  uint8_t rx_dma;

  uint8_t tx_fifo[TX_FIFO_SIZE];
  uint8_t rx_fifo[RX_FIFO_SIZE];

} nrf54l_uart_t;


static void
start_tx(nrf54l_uart_t *u, uint8_t c)
{
  u->tx_dma = c;
  reg_wr(u->base + UARTE_EVENT_ENDTX, 0);
  reg_wr(u->base + UARTE_DMA_TX_PTR, (uint32_t)&u->tx_dma);
  reg_wr(u->base + UARTE_DMA_TX_MAXCNT, 1);
  reg_wr(u->base + UARTE_TASK_STARTTX, 1);
}


static void
arm_rx(nrf54l_uart_t *u)
{
  reg_wr(u->base + UARTE_EVENT_ENDRX, 0);
  reg_wr(u->base + UARTE_DMA_RX_PTR, (uint32_t)&u->rx_dma);
  reg_wr(u->base + UARTE_DMA_RX_MAXCNT, 1);
  reg_wr(u->base + UARTE_TASK_STARTRX, 1);
}


// Busy-wait transmit of a single byte. Used when we can't sleep
// (interrupt context, panic, all IRQs disabled).
static void
printchar(nrf54l_uart_t *u, char c)
{
  u->tx_dma = c;
  reg_wr(u->base + UARTE_EVENT_ENDTX, 0);
  reg_wr(u->base + UARTE_DMA_TX_PTR, (uint32_t)&u->tx_dma);
  reg_wr(u->base + UARTE_DMA_TX_MAXCNT, 1);
  reg_wr(u->base + UARTE_TASK_STARTTX, 1);
  while(!reg_rd(u->base + UARTE_EVENT_ENDTX)) {
  }
  reg_wr(u->base + UARTE_EVENT_ENDTX, 0);
}


static ssize_t
nrf54l_uart_write(stream_t *s, const void *buf, size_t size, int flags)
{
  nrf54l_uart_t *u = (nrf54l_uart_t *)s;
  if(size == 0)
    return 0;

  const char *d = buf;

  const int busy_wait = !can_sleep();

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(busy_wait) {
    // Drain the FIFO first to preserve ordering, then emit our bytes
    while(1) {
      uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
      if(!avail)
        break;
      char c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      u->tx_fifo_rdptr++;
      printchar(u, c);
    }
    for(size_t i = 0; i < size; i++)
      printchar(u, d[i]);
    irq_permit(q);
    return size;
  }

  size_t written = 0;

  for(size_t i = 0; i < size; i++) {
    const uint8_t c = d[i];
    while(1) {
      uint8_t avail = TX_FIFO_SIZE - (u->tx_fifo_wrptr - u->tx_fifo_rdptr);
      if(avail == 0) {
        if(flags & STREAM_WRITE_NO_WAIT)
          break;
        assert(u->tx_busy);
        task_sleep(&u->uart_tx);
        continue;
      }

      written++;
      if(!u->tx_busy) {
        u->tx_busy = 1;
        start_tx(u, c);
        break;
      }

      u->tx_fifo[u->tx_fifo_wrptr & (TX_FIFO_SIZE - 1)] = c;
      u->tx_fifo_wrptr++;
      break;
    }
  }
  irq_permit(q);
  return written;
}


static void
nrf54l_uart_irq(void *arg)
{
  nrf54l_uart_t *u = arg;

  if(reg_rd(u->base + UARTE_EVENT_ENDRX)) {
    char c = u->rx_dma;
    arm_rx(u);

    if(c == 4 && (u->uart_flags & UART_CTRLD_IS_PANIC)) {
      panic("Halted from console");
    }
    u->rx_fifo[u->rx_fifo_wrptr & (RX_FIFO_SIZE - 1)] = c;
    u->rx_fifo_wrptr++;
    task_wakeup(&u->uart_rx, 1);
  }

  if(reg_rd(u->base + UARTE_EVENT_ENDTX)) {
    reg_wr(u->base + UARTE_EVENT_ENDTX, 0);

    uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
    if(avail == 0) {
      u->tx_busy = 0;
    } else {
      char c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      u->tx_fifo_rdptr++;
      task_wakeup(&u->uart_tx, 1);
      start_tx(u, c);
    }
  }
}


static ssize_t
nrf54l_uart_read(struct stream *s, void *buf, size_t size, size_t requested)
{
  nrf54l_uart_t *u = (nrf54l_uart_t *)s;

  uint8_t *d = buf;

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  for(size_t i = 0; i < size; i++) {
    while(u->rx_fifo_wrptr == u->rx_fifo_rdptr) {
      if(i >= requested) {
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


static const stream_vtable_t nrf54l_uart_vtable = {
  .read = nrf54l_uart_read,
  .write = nrf54l_uart_write
};


struct stream *
nrf54l_uart_init(uint32_t base, int irq, int baudrate,
                 gpio_t txpin, gpio_t rxpin, int flags)
{
  nrf54l_uart_t *u = calloc(1, sizeof(nrf54l_uart_t));

  u->stream.vtable = &nrf54l_uart_vtable;
  u->base = base;
  u->uart_flags = flags;

  // PSEL: PIN[4:0], PORT[7:5], CONNECT[31] (0 = connected). gpio_t already
  // encodes (port << 5) | pin, so write it directly with CONNECT cleared.
  reg_wr(base + UARTE_PSEL_TXD, txpin);
  reg_wr(base + UARTE_PSEL_RXD, rxpin);

  // 115200 baud (value is referenced to the fixed 16 MHz UARTE clock)
  reg_wr(base + UARTE_BAUDRATE, 0x01d60000);
  reg_wr(base + UARTE_ENABLE, UARTE_ENABLE_ENABLED);

  irq_enable_fn_arg(irq, IRQ_LEVEL_CONSOLE, nrf54l_uart_irq, u);

  reg_wr(base + UARTE_INTENSET, UARTE_INTEN_ENDRX | UARTE_INTEN_ENDTX);
  arm_rx(u);

  return &u->stream;
}
