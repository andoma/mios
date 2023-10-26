

#define CR1_IDLE \
  (USART_CR1_UE | USART_CR1_RXNEIE | USART_CR1_TE | USART_CR1_RE)

#define CR1_ENABLE_TCIE  (CR1_IDLE | USART_CR1_TCIE)
#define CR1_ENABLE_TXEIE (CR1_IDLE | USART_CR1_TXEIE)


// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file

#include <sys/param.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "irq.h"

#include "stm32_uart_stream.h"


static void
stm32_uart_write(stream_t *s, const void *buf, size_t size)
{
  if(size == 0)
    return;

  stm32_uart_stream_t *u = (stm32_uart_stream_t *)s;
  const char *d = buf;

  const int busy_wait = !can_sleep();

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(busy_wait) {
    // We are not on user thread, busy wait

    while(!(reg_rd(u->reg_base + USART_SR) & (1 << 7))) {}

    while(1) {
      uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
      if(!avail)
        break;
      uint8_t c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      u->tx_fifo_rdptr++;
      reg_wr(u->reg_base + USART_TDR, c);
      while(!(reg_rd(u->reg_base + USART_SR) & (1 << 7))) {}
    }

    for(size_t i = 0; i < size; i++) {
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
      reg_wr(u->reg_base + USART_CR1, CR1_ENABLE_TXEIE);
      u->tx_busy = 1;
    } else {
      u->tx_fifo[u->tx_fifo_wrptr & (TX_FIFO_SIZE - 1)] = d[i];
      u->tx_fifo_wrptr++;
    }
  }
  irq_permit(q);
}


static int
stm32_uart_read(stream_t *s, void *buf, const size_t size, int mode)
{
  stm32_uart_stream_t *u = (stm32_uart_stream_t *)s;
  char *d = buf;

  if(!can_sleep()) {
    // We are not on user thread, busy wait
    for(size_t i = 0; i < size; i++) {
      while(!(reg_rd(u->reg_base + USART_SR) & (1 << 5))) {
        if(stream_wait_is_done(mode, i, size))
          return i;
      }
      d[i] = reg_rd(u->reg_base + USART_RDR);
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
      task_sleep(&u->wait_rx);
    }

    d[i] = u->rx_fifo[u->rx_fifo_rdptr & (RX_FIFO_SIZE - 1)];
    u->rx_fifo_rdptr++;
  }
  irq_permit(q);
  return size;
}




static void
uart_irq(void *arg)
{
  stm32_uart_stream_t *u = arg;

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

  if(u->tx_dma == STM32_DMA_INSTANCE_NONE) {

    if(sr & (1 << 7)) {

      uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
      if(avail == 0) {
        u->tx_busy = 0;
        reg_wr(u->reg_base + USART_CR1, CR1_IDLE);
      } else {
        uint8_t c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
        u->tx_fifo_rdptr++;
        reg_wr(u->reg_base + USART_TDR, c);
        uint8_t avail = TX_FIFO_SIZE - (u->tx_fifo_wrptr - u->tx_fifo_rdptr);
        if(avail > 3 * TX_FIFO_SIZE / 4)
          task_wakeup(&u->wait_tx, 1);
      }
    }

  } else {
#ifdef CR1_ENABLE_TCIE

    if(sr & (1 << 6)) {
      u->tx_busy = 0;
      u->tx_fifo_wrptr = 0;
      task_wakeup(&u->wait_tx, 1);
      reg_clr_bit(u->reg_base + USART_SR, 6); // Clear TC
      stm32_dma_stop(u->tx_dma);
    }
#endif
  }
}



#ifdef CR1_ENABLE_TCIE


static void
stm32_uart_tx_dma_start(stm32_uart_stream_t *u)
{
  if(u->tx_busy)
    return;

  stm32_dma_set_nitems(u->tx_dma, u->tx_fifo_wrptr);

  reg_wr(u->reg_base + USART_CR1, CR1_ENABLE_TCIE);
  stm32_dma_start(u->tx_dma);
  u->tx_busy = 1;
}


static void
stm32_uart_tx_dma_wait(stm32_uart_stream_t *u)
{
  while(u->tx_busy) {
    task_sleep(&u->wait_tx);
  }
}


static void
stm32_uart_write_dma(stream_t *s, const void *buf, size_t size)
{
  stm32_uart_stream_t *u = (stm32_uart_stream_t *)s;

  if(!can_sleep())
    return;

  int q = irq_forbid(IRQ_LEVEL_IO);

  if(size == 0) {

    if(u->tx_fifo_wrptr)
      stm32_uart_tx_dma_start(u);

  } else {

    while(size) {

      stm32_uart_tx_dma_wait(u);

      if(u->tx_fifo_wrptr == TX_FIFO_SIZE) {
        stm32_uart_tx_dma_start(u);
        continue;
      }

      uint8_t avail = TX_FIFO_SIZE - u->tx_fifo_wrptr;
      int to_copy = MIN(avail, size);
      memcpy(u->tx_fifo + u->tx_fifo_wrptr, buf, to_copy);

      buf += to_copy;
      size -= to_copy;
      u->tx_fifo_wrptr += to_copy;
    }

    if(u->tx_fifo_wrptr == TX_FIFO_SIZE)
      stm32_uart_tx_dma_start(u);
  }
  irq_permit(q);
}






#endif


stm32_uart_stream_t *
stm32_uart_stream_init(stm32_uart_stream_t *u, int reg_base, int baudrate,
                       int clkid, int irq, uint8_t flags,
                       uint32_t tx_dma_resouce_id)
{
  if(u == NULL) {
    u = xalloc(sizeof(stm32_uart_stream_t), 0,
               flags & UART_TXDMA ? MEM_TYPE_DMA : 0);
    memset(u, 0, sizeof(stm32_uart_stream_t));
  }
  clk_enable(clkid);

  u->reg_base = reg_base;
  u->flags = flags;

  const unsigned int freq = clk_get_freq(clkid);
  const unsigned int bbr = (freq + baudrate - 1) / baudrate;

  reg_wr(u->reg_base + USART_BRR, bbr);
  reg_wr(u->reg_base + USART_CR1, CR1_IDLE);

  task_waitable_init(&u->wait_rx, "uartrx");
  task_waitable_init(&u->wait_tx, "uarttx");

  u->tx_dma = STM32_DMA_INSTANCE_NONE;
  u->stream.write = stm32_uart_write;

#ifdef CR1_ENABLE_TCIE

  if(flags & UART_TXDMA) {

    u->tx_dma = stm32_dma_alloc(tx_dma_resouce_id, "uart");

    stm32_dma_config(u->tx_dma,
                     STM32_DMA_BURST_NONE,
                     STM32_DMA_BURST_NONE,
                     STM32_DMA_PRIO_LOW,
                     STM32_DMA_8BIT,
                     STM32_DMA_8BIT,
                     STM32_DMA_INCREMENT,
                     STM32_DMA_FIXED,
                     STM32_DMA_SINGLE,
                     STM32_DMA_M_TO_P);

    stm32_dma_set_paddr(u->tx_dma, u->reg_base + USART_TDR);
    stm32_dma_set_mem0(u->tx_dma, u->tx_fifo);

    reg_set_bit(u->reg_base + USART_CR3, 7);
    u->stream.write = stm32_uart_write_dma;
  }
#endif

  irq_enable(irq, IRQ_LEVEL_CONSOLE);

  u->stream.read = stm32_uart_read;

  return u;
}

