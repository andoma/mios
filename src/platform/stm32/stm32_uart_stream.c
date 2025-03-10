// This file is not compiled on its own but needs to be included
// by a stm32 chip specific file

#include <sys/param.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include "irq.h"

#include "stm32_uart_stream.h"
#include "stm32_uart.h"

static void
stm32_uart_update_cr1(stm32_uart_stream_t *u)
{
  uint32_t cr1 = USART_CR1_UE | USART_CR1_RXNEIE | USART_CR1_TE | USART_CR1_RE;

  if(u->tx_busy)
    cr1 |= USART_CR1_TXEIE;
  if(u->tx_enabled)
    cr1 |= USART_CR1_TCIE;

  reg_wr(u->reg_base + USART_CR1, cr1);
}

static task_waitable_t *
stm32_uart_poll(stream_t *s, poll_type_t type)
{
  stm32_uart_stream_t *u = (stm32_uart_stream_t *)s;

  irq_forbid(IRQ_LEVEL_CONSOLE);

  if(type == POLL_STREAM_WRITE) {

    if(TX_FIFO_SIZE - (u->tx_fifo_wrptr - u->tx_fifo_rdptr))
      return NULL;
    return &u->wait_tx;

  } else {

    if(u->rx_fifo_wrptr != u->rx_fifo_rdptr)
      return NULL;
    return &u->wait_rx;
  }
}

static ssize_t
stm32_uart_write(stream_t *s, const void *buf, size_t size, int flags)
{
  if(size == 0)
    return 0;

  stm32_uart_stream_t *u = (stm32_uart_stream_t *)s;
  const char *d = buf;

  const int busy_wait = !can_sleep();

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(busy_wait) {
    // We are not on user thread, busy wait

    while(!(reg_rd(u->reg_base + USART_SR) & UART_SR_TXE)) {}

    while(1) {
      uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
      if(!avail)
        break;
      uint8_t c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      u->tx_fifo_rdptr++;
      reg_wr(u->reg_base + USART_TDR, c);
      while(!(reg_rd(u->reg_base + USART_SR) & UART_SR_TXE)) {}
    }

    for(size_t i = 0; i < size; i++) {
      reg_wr(u->reg_base + USART_TDR, d[i]);
      while(!(reg_rd(u->reg_base + USART_SR) & UART_SR_TXE)) {}
    }
    irq_permit(q);
    return size;
  }

  ssize_t written = 0;

  for(size_t i = 0; i < size; i++) {

    while(1) {
      uint8_t avail = TX_FIFO_SIZE - (u->tx_fifo_wrptr - u->tx_fifo_rdptr);

      if(avail)
        break;
      if(flags & STREAM_WRITE_NO_WAIT)
        goto done;
      assert(u->tx_busy);
      task_sleep(&u->wait_tx);
    }

    if(!u->tx_busy) {
      reg_wr(u->reg_base + USART_TDR, d[i]);
      u->tx_busy = 1;

      if(u->tx_enable != GPIO_UNUSED && !u->tx_enabled) {
        gpio_set_output(u->tx_enable, !u->tx_pol_invert);
        if(u->rx_enable != GPIO_UNUSED)
          gpio_set_output(u->rx_enable, u->rx_pol_invert);

        u->tx_enabled = 1;
      }

      stm32_uart_update_cr1(u);

    } else {
      u->tx_fifo[u->tx_fifo_wrptr & (TX_FIFO_SIZE - 1)] = d[i];
      u->tx_fifo_wrptr++;
    }
    written++;
  }
 done:
  irq_permit(q);
  return written;
}


static ssize_t
stm32_uart_read(stream_t *s, void *buf, const size_t size, size_t requested)
{
  stm32_uart_stream_t *u = (stm32_uart_stream_t *)s;
  char *d = buf;

  if(!can_sleep()) {
    // We are not on user thread, busy wait
    for(size_t i = 0; i < size; i++) {
      while(!(reg_rd(u->reg_base + USART_SR) & (1 << 5))) {
        if(i >= requested)
          return i;
      }
      d[i] = reg_rd(u->reg_base + USART_RDR);
    }
    return size;
  }

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  for(size_t i = 0; i < size; i++) {
    while(u->rx_fifo_wrptr == u->rx_fifo_rdptr) {
      if(i >= requested) {
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

  if(sr & UART_SR_RXNE) {
    const uint8_t c = reg_rd(u->reg_base + USART_RDR);

    if(u->flags & UART_CTRLD_IS_PANIC && c == 4) {
      panic("Halted from console");
    }
    u->rx_fifo[u->rx_fifo_wrptr & (RX_FIFO_SIZE - 1)] = c;
    u->rx_fifo_wrptr++;

    task_wakeup(&u->wait_rx, 1);

    if(sr & UART_SR_FRAMING_ERR) {
      u->rx_framing_error++;
#ifdef USART_ICR
      reg_wr(u->reg_base + USART_ICR, (1 << 1));
#endif
    }
    if(sr & UART_SR_NOISE_ERR) {
      u->rx_noise++;
#ifdef USART_ICR
      reg_wr(u->reg_base + USART_ICR, (1 << 2));
#endif
    }
    if(sr & UART_SR_OVERRUN_ERR) {
      u->rx_overrun++;
#ifdef USART_ICR
      reg_wr(u->reg_base + USART_ICR, (1 << 3));
#endif
    }
  }

  if(sr & UART_SR_TC) {
    if(u->tx_enable != GPIO_UNUSED) {
      gpio_set_output(u->tx_enable, u->tx_pol_invert);
      if(u->rx_enable != GPIO_UNUSED)
        gpio_set_output(u->rx_enable, !u->rx_pol_invert);

      u->tx_enabled = 0;
    }
  }

  if(sr & UART_SR_TXE) { // TX reg empty
    uint8_t avail = u->tx_fifo_wrptr - u->tx_fifo_rdptr;
    if(avail == 0) {
      u->tx_busy = 0;
    } else {
      uint8_t c = u->tx_fifo[u->tx_fifo_rdptr & (TX_FIFO_SIZE - 1)];
      u->tx_fifo_rdptr++;
      reg_wr(u->reg_base + USART_TDR, c);
      uint8_t avail = TX_FIFO_SIZE - (u->tx_fifo_wrptr - u->tx_fifo_rdptr);
      if(avail > 3 * TX_FIFO_SIZE / 4)
        task_wakeup(&u->wait_tx, 1);
    }
  }
  stm32_uart_update_cr1(u);
}

static void
stm32_uart_print_info(struct device *dev, struct stream *st)
{
  stm32_uart_stream_t *u = (void *)dev - offsetof(stm32_uart_stream_t, device);
  stprintf(st, "\tOverrun:%d Noise:%d Framing:%d\n",
           u->rx_overrun,
           u->rx_noise,
           u->rx_framing_error);
}

void
stm32_uart_set_io_ctrl(stream_t *s, gpio_t tx_enable, int tx_pol_invert,
                       gpio_t rx_enable, int rx_pol_invert)
{
  stm32_uart_stream_t *u = (stm32_uart_stream_t *)s;
  u->tx_enable = tx_enable;
  u->tx_pol_invert = !!tx_pol_invert;
  u->rx_enable = rx_enable;
  u->rx_pol_invert = !!rx_pol_invert;
}

static const device_class_t stm32_uart_class = {
  .dc_print_info = stm32_uart_print_info,
};

static const stream_vtable_t stm32_uart_vtable = {
  .write = stm32_uart_write,
  .read = stm32_uart_read,
  .poll = stm32_uart_poll
};

stm32_uart_stream_t *
stm32_uart_stream_init(stm32_uart_stream_t *u, int reg_base, int baudrate,
                       int clkid, int irq, uint8_t flags, const char *name)
{
  if(u == NULL)
    u = calloc(1, sizeof(stm32_uart_stream_t));

  u->tx_enable = GPIO_UNUSED;
  u->rx_enable = GPIO_UNUSED;

  clk_enable(clkid);

  u->device.d_name = name;
  u->device.d_class = &stm32_uart_class;
  device_register(&u->device);

  u->reg_base = reg_base;
  u->flags = flags;

  const unsigned int freq = clk_get_freq(clkid);
  const unsigned int bbr = (freq + baudrate - 1) / baudrate;

  reg_wr(u->reg_base + USART_BRR, bbr);
  stm32_uart_update_cr1(u);

  task_waitable_init(&u->wait_rx, "uartrx");
  task_waitable_init(&u->wait_tx, "uarttx");

  u->tx_dma = STM32_DMA_INSTANCE_NONE;
  u->stream.vtable = &stm32_uart_vtable;

  if(flags & UART_HALF_DUPLEX)
    reg_set_bit(reg_base + USART_CR3, 3); // HDSEL

  _Static_assert(UART_1_5_STOP_BITS == 3);
  reg_set_bits(u->reg_base + USART_CR2, 12, 2, flags & UART_1_5_STOP_BITS);

  /* Clear TC (transmission complete) flag which is set in SR by default.
   * Although the docs state that clearing should be a SR read followed by a DR
   * write, that sequence doesn't seem to work but a simple read does. */
  reg_rd(u->reg_base + USART_SR);

  irq_enable(irq, IRQ_LEVEL_CONSOLE);


  return u;
}
