#include <stdio.h>
#include <mios/task.h>

#include "irq.h"


static volatile unsigned int * const UART_DR    = (unsigned int *)0x4000c000;
static volatile unsigned int * const UART_IMSC  = (unsigned int *)0x4000c038;
//static volatile unsigned int * const UART_RIS   = (unsigned int *)0x4000c03c;

static task_waitable_t uart_rx;

static uint8_t rx_fifo_rdptr;
static uint8_t rx_fifo_wrptr;

#define RX_FIFO_SIZE 64
static uint8_t rx_fifo[RX_FIFO_SIZE];


void
irq_5(void)
{
  uint8_t ch = *UART_DR;
  rx_fifo[rx_fifo_wrptr & (RX_FIFO_SIZE - 1)] = ch;
  rx_fifo_wrptr++;
  task_wakeup(&uart_rx, 1);
}


static ssize_t
uart_read(struct stream *s, void *buf, size_t size, size_t reqsize)
{
  char *d = buf;

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  for(size_t i = 0; i < size; i++) {
    while(1) {
      uint8_t avail = rx_fifo_wrptr - rx_fifo_rdptr;
      if(avail)
        break;
      if(i >= reqsize) {
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


static ssize_t
uart_write(struct stream *s, const void *buf, size_t size, int flags)
{
  const char *d = buf;
  for(size_t i = 0; i < size; i++) {
    *UART_DR = d[i];
  }
  return size;
}


static stream_t uart_stdio = { uart_read, uart_write };

static void __attribute__((constructor(110)))
board_init_console(void)
{
  irq_enable(5, IRQ_LEVEL_CONSOLE);
  *UART_IMSC = 0x10;

  stdio = &uart_stdio;
}
