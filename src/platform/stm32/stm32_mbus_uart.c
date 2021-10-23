#include <stdio.h>
#include <unistd.h>

#include "platform/stm32/stm32_tim.h"

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


static void
start_tx(uart_mbus_t *um)
{
  gpio_set_output(um->txe, 1);
  um->state = MBUS_STATE_TX_SOF;
  um->txoff = 0;
  start_timer(um);
  reg_wr(um->uart_reg_base + USART_TDR, 0x7e);
}


static void
stop_tx(uart_mbus_t *um)
{
  um->state = MBUS_STATE_GAP;
  gpio_set_output(um->txe, 0);
}


static void
tx_byte(uart_mbus_t *um, uint8_t xor)
{
  pbuf_t *pb = STAILQ_FIRST(&um->tx_queue);
  if(um->txoff == pb->pb_pktlen) {
    reg_wr(um->uart_reg_base + USART_TDR, 0x7e);
    um->state = MBUS_STATE_TX_EOF;
    return;
  }

  const uint8_t *d = pbuf_cdata(pb, um->txoff);
  const uint8_t byte = *d ^ xor;

  if(byte == 0x7d || byte == 0x7e) {
    reg_wr(um->uart_reg_base + USART_TDR, 0x7d);
    um->state = MBUS_STATE_TX_FLAG;
    return;
  }

  reg_wr(um->uart_reg_base + USART_TDR, byte);
  um->state = xor ? MBUS_STATE_TX_ESC : MBUS_STATE_TX_DAT;
  um->txoff++;
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
  tx_byte(um, 0);
}


static uint8_t recbuf[256];
static uint8_t recptr;

#include <mios/cli.h>

static int
cmd_recbuf(cli_t *cli, int argc, char **argv)
{
  for(int i = 0; i < 256; i++) {
    uint8_t p = i + recptr;
    cli_printf(cli, "%02x%s\n", recbuf[p], recbuf[p] == 0x7e ? " ====" : "");
  }
  return 0;
}

CLI_CMD_DEF("recbuf", cmd_recbuf);

static void
uart_mbus_rxbyte(uart_mbus_t *um, uint8_t c)
{
  recbuf[recptr++] = c;

  switch(um->state) {
  case MBUS_STATE_IDLE:
  case MBUS_STATE_GAP:
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
    break;

  case MBUS_STATE_TX_EOF:
    if(c == 0x7e) {
      pbuf_free_irq_blocked(pbuf_splice(&um->tx_queue));
    }
    stop_tx(um);
    break;

  case MBUS_STATE_TX_FLAG:
    if(c != 0x7d) {
      stop_tx(um);
      break;
    }
    tx_byte(um, 0x20);
    break;

  case MBUS_STATE_TX_SOF:
    if(c != 0x7e) {
      stop_tx(um);
      break;
    }
    tx_byte(um, 0);
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
  start_timer(um);
}


static void
uart_mbus_irq(void *arg)
{
  uart_mbus_t *um = arg;
  uint32_t sr = reg_rd(um->uart_reg_base + USART_SR);

  if(sr & (1 << 5)) {

    int err = sr & 6;
    const uint8_t c = reg_rd(um->uart_reg_base + USART_RDR);
    reg_wr(um->uart_reg_base + USART_ICR, (1 << 11) | err);

    if(!err) {
      uart_mbus_rxbyte(um, c);
    }
    sr &= ~(1 << 5);
  }

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
#if 0
  sr &= ~0x1000;   // end-of-block
  sr &= ~0x10000;   // Busy line
  sr &= ~0x10;   // Idle line
  sr &= ~0xc0;   // TX stuff
  sr &= ~0x620000; // Character match

  if(sr)
    panic("sr=%x\n", sr);
#endif
}



static void
uart_mbus_output(struct mbus_netif *mni, pbuf_t *pb)
{
  uart_mbus_t *um = (uart_mbus_t *)mni;
  int q = irq_forbid(IRQ_LEVEL_NET);
  STAILQ_INSERT_TAIL(&um->tx_queue, pb, pb_link);
  if(um->state == MBUS_STATE_IDLE)
    start_tx(um);
  irq_permit(q);
}



static void
timer_irq(void *arg)
{
  uart_mbus_t *um = arg;
  reg_wr(um->tim_reg_base + TIMx_SR, 0);
  if(STAILQ_FIRST(&um->tx_queue) != NULL) {
    start_tx(um);
  } else {
    um->state = MBUS_STATE_IDLE;
  }
}


void
stm32_mbus_uart_create(uint32_t uart_reg_base, int baudrate,
                       int clkid, int uart_irq, uint32_t tx_dma_resouce_id,
                       gpio_t txe, uint8_t local_addr,
                       const stm32_timer_info_t *tim,
                       uint8_t prio)
{
  clk_enable(clkid);

  uart_mbus_t *um = calloc(1, sizeof(uart_mbus_t));

  STAILQ_INIT(&um->tx_queue);
  um->um_mni.mni_hdr_len = 1;
  um->um_mni.mni_output = uart_mbus_output;
  mbus_netif_attach(&um->um_mni, "uartmbus", local_addr);

  const unsigned int freq = clk_get_freq(clkid);
  const unsigned int bbr = (freq + baudrate - 1) / baudrate;

  um->uart_reg_base = uart_reg_base;
  reg_wr(um->uart_reg_base + USART_BBR, bbr);
  reg_wr(um->uart_reg_base + USART_CR1, CR1_IDLE);
#if 0
  reg_wr(um->uart_reg_base + USART_CR1, CR1_IDLE | 2);
  reg_wr(um->uart_reg_base + USART_CR3, 0b110 << 20);
#endif
  um->tim_reg_base = tim->base;
  um->state = MBUS_STATE_GAP;
  um->txe = txe;
  um->prio = prio;
  clk_enable(tim->clk);
  irq_enable_fn_arg(tim->irq, IRQ_LEVEL_NET, timer_irq, um);
  irq_enable_fn_arg(uart_irq, IRQ_LEVEL_NET, uart_mbus_irq, um);

  reg_wr(um->tim_reg_base + TIMx_PSC, (16000000 / 1000000) - 1);
  reg_wr(um->tim_reg_base + TIMx_CNT, 0);
  reg_wr(um->tim_reg_base + TIMx_ARR, 10000 - 1);
  reg_wr(um->tim_reg_base + TIMx_CR1, 0b1001);
  reg_wr(um->tim_reg_base + TIMx_DIER, 0x1);
}
