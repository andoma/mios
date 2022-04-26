#include <unistd.h>

#include "platform/stm32/stm32_tim.h"

#include <mios/suspend.h>

#include "net/mbus/mbus.h"
#include "irq.h"

#define MBUS_STATE_IDLE     0
#define MBUS_STATE_GAP      1
#define MBUS_STATE_RX       2
#define MBUS_STATE_RX_ESC   3
#define MBUS_STATE_RX_NOBUF 4

#define MBUS_STATE_TX_SOF   5
#define MBUS_STATE_TX_DAT   6
#define MBUS_STATE_TX_FLAG  7
#define MBUS_STATE_TX_ESC   8
#define MBUS_STATE_TX_EOF   9


typedef struct uart_mbus {
  mbus_netif_t um_mni;
  struct pbuf_queue tx_queue;
  struct pbuf *rx_pbuf;

  uint32_t uart_reg_base;
  uint32_t tim_reg_base;

  uint8_t state;
  uint8_t txoff;
  uint8_t prio;
  uint8_t flags;
  uint8_t tx_state;
  uint8_t tx_queue_len;
  uint8_t tx_attempts;

  gpio_t txe;

} uart_mbus_t;


static void
start_timer(uart_mbus_t *um)
{
  reg_wr(um->tim_reg_base + TIMx_CNT, 0);

  int timeout = 2000 + um->prio * 100 + (rand() & 2047);
  reg_wr(um->tim_reg_base + TIMx_ARR, timeout);
  reg_wr(um->tim_reg_base + TIMx_CR1, 0b1101);
}


static uint8_t
tx_byte(uart_mbus_t *um, uint8_t xor)
{
  pbuf_t *pb = STAILQ_FIRST(&um->tx_queue);
  if(um->txoff == pb->pb_pktlen) {
    reg_wr(um->uart_reg_base + USART_TDR, 0x7e);
    return MBUS_STATE_TX_EOF;
  }

  const uint8_t *d = pbuf_cdata(pb, um->txoff);
  const uint8_t byte = *d ^ xor;

  if(byte == 0x7d || byte == 0x7e) {
    reg_wr(um->uart_reg_base + USART_TDR, 0x7d);
    return MBUS_STATE_TX_FLAG;
  }

  reg_wr(um->uart_reg_base + USART_TDR, byte);
  um->txoff++;
  return xor ? MBUS_STATE_TX_ESC : MBUS_STATE_TX_DAT;
}


static void
splice(uart_mbus_t *um)
{
  um->tx_queue_len--;
  um->tx_attempts = 0;
  pbuf_free_irq_blocked(pbuf_splice(&um->tx_queue));
}

static void
start_tx_hd(uart_mbus_t *um)
{
  um->tx_attempts++;

  if(um->tx_attempts == 100) {
    um->um_mni.mni_tx_fail++;
    splice(um);
  }
  start_timer(um);

#ifdef USART_SR_BUSY
  uint32_t sr = reg_rd(um->uart_reg_base + USART_SR);
  if(sr & USART_SR_BUSY)
    return;
#endif
  gpio_set_output(um->txe, 1);
  um->state = MBUS_STATE_TX_SOF;
  um->txoff = 0;
  reg_wr(um->uart_reg_base + USART_TDR, 0x7e);
}



static void
start_tx_fd(uart_mbus_t *um)
{
  um->tx_state = MBUS_STATE_TX_SOF;
  um->txoff = 0;
  reg_wr(um->uart_reg_base + USART_TDR, 0x7e);
  reg_wr(um->uart_reg_base + USART_CR1, CR1_ENABLE_TXI);
}



static void
stop_tx(uart_mbus_t *um)
{
  um->state = MBUS_STATE_GAP;
  gpio_set_output(um->txe, 0);
}


static void
rx_and_tx(uart_mbus_t *um, uint8_t c, uint8_t xor)
{
  const pbuf_t *pb = STAILQ_FIRST(&um->tx_queue);
  const uint8_t *d = pbuf_cdata(pb, um->txoff - 1);
  if((c ^ xor) != *d) {
    stop_tx(um);
    return;
  }
  um->state = tx_byte(um, 0);
}


static void
uart_mbus_rxbyte(uart_mbus_t *um, uint8_t c)
{
  switch(um->state) {
  case MBUS_STATE_IDLE:
    if(um->flags & UART_WAKEUP)
      wakelock_acquire();
    // FALLTHRU
  case MBUS_STATE_GAP:
  rx:
    um->state = MBUS_STATE_RX;
    // FALLTHRU
  case MBUS_STATE_RX:

    if(c == 0x7e) {
      if(um->rx_pbuf != NULL) {
        if(um->rx_pbuf->pb_pktlen == 0)
          break;
        STAILQ_INSERT_TAIL(&um->um_mni.mni_ni.ni_rx_queue, um->rx_pbuf,
                           pb_link);
        netif_wakeup(&um->um_mni.mni_ni);
      }
      um->rx_pbuf = pbuf_make_irq_blocked(0, 0);
      if(um->rx_pbuf == NULL) {
        um->state = MBUS_STATE_RX_NOBUF;
        return;
      }
      break;
    }
    if(c == 0x7d) {
      um->state = MBUS_STATE_RX_ESC;
      break;
    }

    if(0) {
  case MBUS_STATE_RX_ESC:
      c ^= 0x20;
      um->state = MBUS_STATE_RX;
    }

    if(um->rx_pbuf == NULL)
      return;

    uint8_t *buf = pbuf_append(um->rx_pbuf, 1);
    if(buf == NULL) {
      pbuf_free(um->rx_pbuf);
      um->rx_pbuf = NULL;
      um->state = MBUS_STATE_RX_NOBUF;
      return;
    }
    *buf = c;
    break;

  case MBUS_STATE_RX_NOBUF:
    if(c != 0x7e)
      break;
    goto rx;

  case MBUS_STATE_TX_EOF:
    if(c == 0x7e) {
      um->um_mni.mni_tx_packets++;
      splice(um);
    }
    stop_tx(um);
    break;

  case MBUS_STATE_TX_FLAG:
    if(c != 0x7d) {
      stop_tx(um);
      break;
    }
    um->state = tx_byte(um, 0x20);
    break;

  case MBUS_STATE_TX_SOF:
    if(c != 0x7e) {
      stop_tx(um);
      break;
    }
    um->state = tx_byte(um, 0);
    break;

  case MBUS_STATE_TX_ESC:
    rx_and_tx(um, c, 0x20);
    break;

  case MBUS_STATE_TX_DAT:
    rx_and_tx(um, c, 0x0);
    break;

  default:
    panic("RX in state %d", um->state);
  }
}

static void
uart_mbus_txirq(uart_mbus_t *um)
{
  switch(um->tx_state) {
  case MBUS_STATE_TX_EOF:
    um->tx_queue_len--;
    pbuf_free_irq_blocked(pbuf_splice(&um->tx_queue));
    if(STAILQ_FIRST(&um->tx_queue)) {
      start_tx_fd(um);
    } else {
      um->tx_state = MBUS_STATE_IDLE;
      reg_wr(um->uart_reg_base + USART_CR1, CR1_IDLE);
    }
    break;

  case MBUS_STATE_TX_FLAG:
    um->tx_state = tx_byte(um, 0x20);
    break;

  case MBUS_STATE_TX_SOF:
  case MBUS_STATE_TX_ESC:
  case MBUS_STATE_TX_DAT:
    um->tx_state = tx_byte(um, 0);
    break;

  default:
    panic("TX in state %d", um->tx_state);
  }
}





static void
uart_mbus_irq(void *arg)
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
      uart_mbus_rxbyte(um, c);
      if(um->tim_reg_base)
        start_timer(um);
    }
    sr &= ~(1 << 5);
  }

  if(sr & (1 << 7) & reg_rd(um->uart_reg_base + USART_CR1)) {
    uart_mbus_txirq(um);
  }

#ifdef USART_ICR
  if(sr & (1 << 1)) {
    sr &= ~(1 << 1);
    reg_wr(um->uart_reg_base + USART_ICR, (1 << 1));
  }

  if(sr & (1 << 2)) {
    sr &= ~(1 << 2);
    reg_wr(um->uart_reg_base + USART_ICR, (1 << 2));
  }
  if(sr & (1 << 3)) {
    reg_wr(um->uart_reg_base + USART_ICR, (1 << 3));
    sr &= ~(1 << 3);
  }
  if(sr & (1 << 20)) {
    reg_wr(um->uart_reg_base + USART_ICR, (1 << 20));
    sr &= ~(1 << 20);
  }
#endif
}



static pbuf_t *
uart_mbus_output_hd(struct mbus_netif *mni, pbuf_t *pb)
{
  uart_mbus_t *um = (uart_mbus_t *)mni;
  int q = irq_forbid(IRQ_LEVEL_NET);
  if(um->tx_queue_len < 5) {
    um->tx_queue_len++;
    STAILQ_INSERT_TAIL(&um->tx_queue, pb, pb_link);
    pb = NULL;
    if(um->state == MBUS_STATE_IDLE) {
      if(um->flags & UART_WAKEUP)
        wakelock_acquire();
      start_tx_hd(um);
    }
  } else {
    mni->mni_tx_qdrops++;
  }
  irq_permit(q);
  return pb;
}

static pbuf_t *
uart_mbus_output_fd(struct mbus_netif *mni, pbuf_t *pb)
{
  uart_mbus_t *um = (uart_mbus_t *)mni;
  int q = irq_forbid(IRQ_LEVEL_NET);
  if(um->tx_queue_len < 5) {
    um->tx_queue_len++;
    STAILQ_INSERT_TAIL(&um->tx_queue, pb, pb_link);
    pb = NULL;
    if(um->tx_state == MBUS_STATE_IDLE) {
      start_tx_fd(um);
    }
  } else {
    mni->mni_tx_qdrops++;
  }
  irq_permit(q);
  return pb;
}



static void
timer_irq(void *arg)
{
  uart_mbus_t *um = arg;
  reg_wr(um->tim_reg_base + TIMx_SR, 0);
  if(STAILQ_FIRST(&um->tx_queue) != NULL) {
    start_tx_hd(um);
  } else {
    um->state = MBUS_STATE_IDLE;

    if(um->flags & UART_WAKEUP)
      wakelock_release();
  }
}


static void
stm32_mbus_uart_print_info(struct device *dev, struct stream *st)
{
  uart_mbus_t *um = (uart_mbus_t *)dev;
  mbus_print_info(&um->um_mni, st);
}


static void
stm32_mbus_uart_power_state(struct device *dev, device_power_state_t state)
{
  uart_mbus_t *um = (uart_mbus_t *)dev;
  if(state == DEVICE_POWER_STATE_SUSPEND) {
    // Make sure we are silent on bus when suspended
    gpio_set_output(um->txe, 0);
  }
}

static const device_class_t stm32_mbus_uart_device_class = {
  .dc_print_info = stm32_mbus_uart_print_info,
  .dc_power_state = stm32_mbus_uart_power_state,
};

static void
stm32_mbus_uart_create(uint32_t uart_reg_base, int bbr,
                       int clkid, int uart_irq, uint32_t tx_dma_resouce_id,
                       gpio_t txe, uint8_t local_addr,
                       const stm32_timer_info_t *tim,
                       uint8_t prio, int flags)
{
  clk_enable(clkid);

  uart_mbus_t *um = calloc(1, sizeof(uart_mbus_t));
  STAILQ_INIT(&um->tx_queue);

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

  um->state = MBUS_STATE_GAP;
  um->txe = txe;
  um->prio = prio;
  um->flags = flags;

  irq_enable_fn_arg(uart_irq, IRQ_LEVEL_NET, uart_mbus_irq, um);

  if(tim != NULL) {

    um->um_mni.mni_output = uart_mbus_output_hd;

    if(flags & UART_WAKEUP)
      wakelock_acquire();

    um->tim_reg_base = tim->base;
    clk_enable(tim->clk);
    irq_enable_fn_arg(tim->irq, IRQ_LEVEL_NET, timer_irq, um);

    reg_wr(um->tim_reg_base + TIMx_PSC, (16000000 / 1000000) - 1);
    reg_wr(um->tim_reg_base + TIMx_CNT, 0);
    reg_wr(um->tim_reg_base + TIMx_ARR, 10000 - 1);
    reg_wr(um->tim_reg_base + TIMx_CR1, 0b1001);
    reg_wr(um->tim_reg_base + TIMx_DIER, 0x1);

  } else {

    um->tim_reg_base = 0;
    um->um_mni.mni_output = uart_mbus_output_fd;
  }

  um->um_mni.mni_ni.ni_mtu = 32;

  mbus_netif_attach(&um->um_mni, "uartmbus", &stm32_mbus_uart_device_class,
                    local_addr, MBUS_NETIF_ENABLE_PCS);
}
