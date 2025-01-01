#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <mios/task.h>
#include <mios/io.h>

#include "irq.h"

#define TX_FIFO_SIZE 128
#define RX_FIFO_SIZE 64

typedef struct a64v_uart {

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

} a64v_uart_t;



volatile unsigned int * const UART0DR = (unsigned int *) 0x09000000;

static void
printchar(char c)
{
  *UART0DR = c;
}


static void
a64v_uart_write(stream_t *s, const void *buf, size_t size, int flags)
{
  const char *d = buf;
  for(size_t i = 0; i < size; i++) {
    printchar(d[i]);
  }
}

static int
a64v_uart_read(struct stream *s, void *buf, size_t size, int mode)
{
  return 0;
}

struct stream *
a64virt_uart_init(int baudrate, gpio_t txpin, gpio_t rxpin, int flags)
{
  a64v_uart_t *u = calloc(1, sizeof(a64v_uart_t));

  u->stream.read = a64v_uart_read;
  u->stream.write = a64v_uart_write;

  return &u->stream;
}
