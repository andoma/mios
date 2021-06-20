#include <assert.h>
#include <stdio.h>

#include <mios/task.h>
#include <mios/io.h>

#include "nrf52_reg.h"

#include "irq.h"

#define UART_BASE 0x40002000

#define UART_INTENSET (UART_BASE + 0x304)
#define UART_ENABLE   (UART_BASE + 0x500)
#define UART_PSELTXD  (UART_BASE + 0x50c)
#define UART_PSELRXD  (UART_BASE + 0x514)
#define UART_RXD      (UART_BASE + 0x518)
#define UART_TXD      (UART_BASE + 0x51c)
#define UART_BAUDRATE (UART_BASE + 0x524)

#define UART_TX_TASK  (UART_BASE + 0x8)
#define UART_TX_RDY   (UART_BASE + 0x11c)

#define UART_RX_TASK  (UART_BASE + 0x0)
#define UART_RX_RDY   (UART_BASE + 0x108)


static task_waitable_t uart_rx = WAITABLE_INITIALIZER("uart_rx");
static task_waitable_t uart_tx = WAITABLE_INITIALIZER("uart_tx");

static uint8_t rx_fifo_rdptr;
static uint8_t rx_fifo_wrptr;
static uint8_t tx_fifo_rdptr;
static uint8_t tx_fifo_wrptr;

#define TX_FIFO_SIZE 128
#define RX_FIFO_SIZE 64

static uint8_t tx_fifo[TX_FIFO_SIZE];
static uint8_t rx_fifo[RX_FIFO_SIZE];

static uint8_t tx_busy;

static void
nrf52_uart_write(stream_t *s, const void *buf, size_t size)
{
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
      uint8_t avail = TX_FIFO_SIZE - (tx_fifo_rdptr - tx_fifo_wrptr);
      if(avail == 0) {
        assert(tx_busy);
        task_sleep(&uart_tx);
        continue;
      }

      if(!tx_busy) {
        reg_wr(UART_TXD, c);
        reg_wr(UART_TX_TASK, 1);
        tx_busy = 1;
        break;
      }

      tx_fifo[tx_fifo_wrptr & (TX_FIFO_SIZE - 1)] = c;
      tx_fifo_wrptr++;
      break;
    }
  }
  irq_permit(q);
}


void
irq_2(void)
{
  if(reg_rd(UART_RX_RDY)) {
    reg_wr(UART_RX_RDY, 0);
    rx_fifo[rx_fifo_wrptr & (RX_FIFO_SIZE - 1)] = reg_rd(UART_RXD);
    rx_fifo_wrptr++;
    task_wakeup(&uart_rx, 1);
  }

  if(reg_rd(UART_TX_RDY)) {
    reg_wr(UART_TX_RDY, 0);

    uint8_t avail = tx_fifo_wrptr - tx_fifo_rdptr;
    if(avail == 0) {
      reg_wr(UART_TX_TASK, 0);
      tx_busy = 0;
    } else {
      char c = tx_fifo[tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      tx_fifo_rdptr++;
      task_wakeup(&uart_tx, 1);
      reg_wr(UART_TXD, c);
    }
  }
}


#if 0
static void *
console_echo_task(void *arg)
{
  int s = irq_forbid(IRQ_LEVEL_CONSOLE);

  while(1) {

    uint8_t avail = rx_fifo_wrptr - rx_fifo_rdptr;
    if(avail == 0) {
      task_sleep(&uart_rx);
      continue;
    }

    char c = rx_fifo[rx_fifo_rdptr & (RX_FIFO_SIZE - 1)];
    rx_fifo_rdptr++;
    uart_putc(NULL, c);
    irq_permit(s);
    s = irq_forbid(IRQ_LEVEL_CONSOLE);
  }
  return NULL;
}
#endif


static int
nrf52_uart_read(struct stream *s, void *buf, size_t size, int mode)
{
  uint8_t *d = buf;

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  for(size_t i = 0; i < size; i++) {
    while(rx_fifo_wrptr == rx_fifo_rdptr) {
      if(stream_wait_is_done(mode, i, size)) {
        irq_permit(q);
        return i;
      }
      task_sleep(&uart_rx);
    }

    d[i] = rx_fifo[rx_fifo_rdptr & (RX_FIFO_SIZE - 1)];
    rx_fifo_rdptr++;
  }
  irq_permit(q);
  return size;
}



static stream_t uart_stream = {
  .read = nrf52_uart_read,
  .write = nrf52_uart_write,
};

struct stream *
nrf52_uart_init(int baudrate, gpio_t txpin, gpio_t rxpin, int flags)
{
  reg_wr(UART_PSELTXD, txpin);
  reg_wr(UART_PSELRXD, rxpin);
  reg_wr(UART_ENABLE, 4);
  reg_wr(UART_BAUDRATE, 0x1d60000);

  reg_wr(UART_INTENSET, 0x84); // RXDRDY and TXDRDY
  reg_wr(UART_RX_TASK, 1);

  irq_enable(2, IRQ_LEVEL_CONSOLE);

  return &uart_stream;
}
