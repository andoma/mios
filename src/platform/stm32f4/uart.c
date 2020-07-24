#include <assert.h>
#include <stdint.h>
#include <task.h>
#include <irq.h>

#include "uart.h"
#include "stm32f4.h"

#include "clk_config.h"


#define USART_SR   0x00
#define USART_DR   0x04
#define USART_BBR  0x08
#define USART_CR1  0x0c


#define CR1_IDLE       (1 << 13) | (1 << 5) | (1 << 3) | (1 << 2)
#define CR1_ENABLE_TXI CR1_IDLE | (1 << 7)

void
uart_putc(void *arg, char c)
{
  uart_t *u = arg;

  int s = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(1 || !can_sleep()) {
    // We not on user thread, busy wait
    while(!(reg_rd(u->reg_base + USART_SR) & (1 << 7))) {}
    reg_wr(u->reg_base + USART_DR, c);
    irq_permit(s);
    return;
  }

  while(1) {
    uint8_t avail = TX_FIFO_SIZE - (u->tx_fifo_wrptr - u->tx_fifo_rdptr);

    if(avail)
      break;
    assert(u->tx_busy);
    task_sleep(&u->wait_tx, 0);
  }

  if(!u->tx_busy) {
    reg_wr(u->reg_base + USART_DR, c);
    reg_wr(u->reg_base + USART_CR1, CR1_ENABLE_TXI);
    u->tx_busy = 1;
  } else {
    u->tx_fifo[u->tx_fifo_wrptr & (TX_FIFO_SIZE - 1)] = c;
    u->tx_fifo_wrptr++;
  }
  irq_permit(s);
}




int
uart_getc(void *arg)
{
  uart_t *u = arg;

  int s = irq_forbid(IRQ_LEVEL_CONSOLE);

  while(u->rx_fifo_wrptr == u->rx_fifo_rdptr)
    task_sleep(&u->wait_rx, 0);

  char c = u->rx_fifo[u->rx_fifo_rdptr & (RX_FIFO_SIZE - 1)];
  u->rx_fifo_rdptr++;
  irq_permit(s);
  return c;
}




void
uart_irq(uart_t *u)
{
  const uint32_t sr = reg_rd(u->reg_base + USART_SR);

  if(sr & (1 << 5)) {
    const uint8_t c = reg_rd(u->reg_base + USART_DR);
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
      reg_wr(u->reg_base + USART_DR, c);
    }
  }
}


void
uart_init(uart_t *u, int reg_base, int baudrate)
{
  const unsigned int bbr = (APB1CLOCK + baudrate - 1) / baudrate;

  u->reg_base = reg_base;
  reg_wr(u->reg_base + USART_CR1, (1 << 13)); // ENABLE
  reg_wr(u->reg_base + USART_BBR, bbr);
  reg_wr(u->reg_base + USART_CR1, CR1_IDLE);
  TAILQ_INIT(&u->wait_rx);
  TAILQ_INIT(&u->wait_tx);
}


