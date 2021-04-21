// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file

#include <assert.h>
#include <stdlib.h>

#include "irq.h"

#include "stm32_uart.h"

void
stm32_uart_write(stream_t *s, const void *buf, size_t size)
{
  stm32_uart_t *u = (stm32_uart_t *)s;
  const char *d = buf;

  const int busy_wait = !can_sleep();

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(busy_wait) {
    // We are not on user thread, busy wait
    for(size_t i = 0; i < size; i++) {
      while(!(reg_rd(u->reg_base + USART_SR) & (1 << 7))) {}
      reg_wr(u->reg_base + USART_TDR, d[i]);
      while(!(reg_rd(u->reg_base + USART_SR) & (1 << 7))) {}
    }
    irq_permit(q);
    return;
  }

  for(size_t i = 0; i < size; i++) {

    while(1) {
      uint8_t avail = TX_FIFO_SIZE - (u->tx_fifo_wrptr - u->tx_fifo_rdptr);

      if(avail)
        break;
      assert(u->tx_busy);
      task_sleep(&u->wait_tx);
    }

    if(!u->tx_busy) {
      reg_wr(u->reg_base + USART_TDR, d[i]);
      reg_wr(u->reg_base + USART_CR1, CR1_ENABLE_TXI);
      u->tx_busy = 1;
    } else {
      u->tx_fifo[u->tx_fifo_wrptr & (TX_FIFO_SIZE - 1)] = d[i];
      u->tx_fifo_wrptr++;
    }
  }
  irq_permit(q);
}


static int
is_done(int mode, size_t done, size_t size)
{
  switch(mode) {
  default:
    return 1;
  case STREAM_READ_WAIT_ONE:
    return done;
  case STREAM_READ_WAIT_ALL:
    return done == size;
  }
}





static int
stm32_uart_read(stream_t *s, void *buf, const size_t size, int mode)
{
  stm32_uart_t *u = (stm32_uart_t *)s;
  char *d = buf;

  if(!can_sleep()) {
    // We are not on user thread, busy wait
    for(size_t i = 0; i < size; i++) {
      while(!(reg_rd(u->reg_base + USART_SR) & (1 << 5))) {
        if(is_done(mode, i, size))
          return i;
      }
      d[i] = reg_rd(u->reg_base + USART_RDR);
    }
    return size;
  }

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  for(size_t i = 0; i < size; i++) {
    while(u->rx_fifo_wrptr == u->rx_fifo_rdptr) {
      if(is_done(mode, i, size)) {
        irq_permit(q);
        return i;
      }
      task_sleep(&u->wait_rx);
    }

    d[i] = u->rx_fifo[u->rx_fifo_rdptr & (RX_FIFO_SIZE - 1)];
    u->rx_fifo_rdptr++;
  }
  irq_permit(q);
  return size;
}




static void
uart_irq(stm32_uart_t *u)
{
  if(u == NULL)
    return;

  const uint32_t sr = reg_rd(u->reg_base + USART_SR);

  if(sr & (1 << 5)) {
    const uint8_t c = reg_rd(u->reg_base + USART_RDR);

    if(u->flags & UART_CTRLD_IS_PANIC && c == 4) {
      panic("Halted from console");
    }
    u->rx_fifo[u->rx_fifo_wrptr & (RX_FIFO_SIZE - 1)] = c;
    u->rx_fifo_wrptr++;

    task_wakeup(&u->wait_rx, 1);
  }

  if(sr & (1 << 7)) {
    uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
    if(avail == 0) {
      u->tx_busy = 0;
      reg_wr(u->reg_base + USART_CR1, CR1_IDLE);
    } else {
      uint8_t c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      u->tx_fifo_rdptr++;
      task_wakeup(&u->wait_tx, 1);
      reg_wr(u->reg_base + USART_TDR, c);
    }
  }
}



stm32_uart_t *
stm32_uart_init(stm32_uart_t *u, int reg_base, int baudrate,
                int clkid, int irq, uint8_t flags)
{
  if(u == NULL)
    u = calloc(1, sizeof(stm32_uart_t));

  clk_enable(clkid);

  u->reg_base = reg_base;
  u->flags = flags;

  const unsigned int freq = clk_get_freq(clkid);
  const unsigned int bbr = (freq + baudrate - 1) / baudrate;

  reg_wr(u->reg_base + USART_BBR, bbr);
  reg_wr(u->reg_base + USART_CR1, CR1_IDLE);

  task_waitable_init(&u->wait_rx, "uartrx");
  task_waitable_init(&u->wait_tx, "uarttx");

  irq_enable(irq, IRQ_LEVEL_CONSOLE);

  u->stream.read = stm32_uart_read;
  u->stream.write = stm32_uart_write;

  return u;
}

