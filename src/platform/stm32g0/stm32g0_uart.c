#include <assert.h>
#include <stdint.h>
#include <mios/task.h>
#include <stdio.h>

#include "stm32g0_uart.h"
#include "stm32g0_clk.h"

#include "irq.h"

#define USART_CR1  0x00
#define USART_BBR  0x0c
#define USART_SR   0x1c
#define USART_RDR  0x24
#define USART_TDR  0x28

#define CR1_IDLE       (1 << 0) | (1 << 5) | (1 << 3) | (1 << 2)
#define CR1_ENABLE_TXI CR1_IDLE | (1 << 7)


void
stm32g0_uart_write(stream_t *s, const void *buf, size_t size)
{
  stm32g0_uart_t *u = (stm32g0_uart_t *)s;
  const char *d = buf;

  const int busy_wait = !can_sleep();

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(busy_wait) {
    // We are not on user thread, busy wait
    for(size_t i = 0; i < size; i++) {
      while(!(reg_rd(u->reg_base + USART_SR) & (1 << 7))) {}
      reg_wr(u->reg_base + USART_TDR, d[i]);
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
stm32g0_uart_read(stream_t *s, void *buf, const size_t size, int mode)
{
  stm32g0_uart_t *u = (stm32g0_uart_t *)s;
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
uart_irq(stm32g0_uart_t *u)
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



static const struct {
  uint16_t base;
  uint16_t clkid;
  uint8_t irq;
  uint8_t af;
} uart_config[] = {
  { 0x0138, CLK_USART1, 27, 1},
  { 0x0044, CLK_USART2, 28, 1},
  { 0x0048, CLK_USART3, 29, 1},
  { 0x004c, CLK_USART4, 29, 1},
  { 0x0050, CLK_USART5, 29, 1},
  { 0x013c, CLK_USART6, 29, 1},
};


static stm32g0_uart_t *uarts[6];

stream_t *
stm32g0_uart_init(stm32g0_uart_t *u, int instance, int baudrate,
                  gpio_t tx, gpio_t rx, uint8_t flags)
{
  if(instance < 1 || instance > 6)
    return NULL;

  instance--;

  const int af = uart_config[instance].af;
  gpio_conf_af(tx, af, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(rx, af, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);

  clk_enable(uart_config[instance].clkid);

  u->reg_base = (uart_config[instance].base << 8) + 0x40000000;
  u->flags = flags;

  const unsigned int freq = 16000000; // clk_get_freq(uart_config[instance].clkid);
  const unsigned int bbr = (freq + baudrate - 1) / baudrate;

  reg_wr(u->reg_base + USART_CR1, (1 << 0)); // ENABLE
  reg_wr(u->reg_base + USART_BBR, bbr);
  reg_wr(u->reg_base + USART_CR1, CR1_IDLE);

  task_waitable_init(&u->wait_rx, "uartrx");
  task_waitable_init(&u->wait_tx, "uarttx");
  uarts[instance] = u;

  irq_enable(uart_config[instance].irq, IRQ_LEVEL_CONSOLE);

  u->stream.read = stm32g0_uart_read;
  u->stream.write = stm32g0_uart_write;
  return &u->stream;
}

void irq_27(void) { uart_irq(uarts[0]); }
void irq_28(void) { uart_irq(uarts[1]); }

//void irq_39(void) { uart_irq(uarts[2]); }
//void irq_52(void) { uart_irq(uarts[3]); }
//void irq_53(void) { uart_irq(uarts[4]); }
//void irq_71(void) { uart_irq(uarts[5]); }
