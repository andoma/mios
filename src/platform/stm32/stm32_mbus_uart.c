#include "lib/mbus/mbus_uart.c"

static void
mbus_uart_tx_byte(uart_mbus_t *um, uint8_t byte)
{
  reg_wr(um->uart_reg_base + USART_TDR, byte);
}


static int
mbus_uart_is_busy(uart_mbus_t *um)
{
#ifdef USART_SR_BUSY
  uint32_t sr = reg_rd(um->uart_reg_base + USART_SR);
  if(sr & USART_SR_BUSY)
    return 1;
#endif
  return 0;
}

static void
stm32_mbus_uart_irq(void *arg)
{
  uart_mbus_t *um = arg;
  uint32_t sr = reg_rd(um->uart_reg_base + USART_SR);

  if(sr & (1 << 5)) {

    int err = sr & 6;
    const uint8_t c = reg_rd(um->uart_reg_base + USART_RDR);
#ifdef USART_ICR
    reg_wr(um->uart_reg_base + USART_ICR, (1 << 11) | err);
#endif
    if(!err) {
      if(!fifo_is_full(&um->rxfifo)) {
        fifo_wr(&um->rxfifo, c);
        softirq_trig(&um->softirq);
      } else {
        um->rx_fifo_full++;
      }
    } else {
      if(err & 2)
        um->rx_framing_error++;
      if(err & 4)
        um->rx_noise_error++;
    }
  }

#ifdef USART_ICR
  const uint32_t clearmask = (1 << 3) | (1 << 20);
  reg_wr(um->uart_reg_base + USART_ICR, sr & clearmask);
#endif
}

static void
stm32_mbus_uart_create(uint32_t uart_reg_base, int bbr,
                       int clkid, int uart_irq, uint32_t tx_dma_resouce_id,
                       gpio_t txe,
                       uint8_t prio, int flags)
{
  clk_enable(clkid);

  uart_mbus_t *um = calloc(1, sizeof(uart_mbus_t));

  um->uart_reg_base = uart_reg_base;
  reg_wr(um->uart_reg_base + USART_BBR, bbr);

  uint32_t cr1 = CR1_IDLE;

#if defined(USART_CR2) && defined(USART_CR3)
  if(flags & UART_WAKEUP) {
    reg_wr(um->uart_reg_base + USART_CR2, 0x7e << 24);
    reg_wr(um->uart_reg_base + USART_CR3, 0b100 << 20);
    cr1 |= 2;
  }
#endif
  reg_wr(um->uart_reg_base + USART_CR1, cr1);

  irq_enable_fn_arg(uart_irq, IRQ_LEVEL_CONSOLE, stm32_mbus_uart_irq, um);

  if(flags & UART_WAKEUP)
    wakelock_acquire();

  mbus_uart_init_common(um, txe, prio, flags);
}
