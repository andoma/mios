#include "net/mbus/mbus.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <mios/suspend.h>

#include "util/crc4.h"

#include "irq.h"

#define MBUS_BAUDRATE_HEADER  115200
#define MBUS_BAUDRATE_PAYLOAD 1000000

#define MBUS_PAYLOAD_BYTE_TIME (10 * 1000000 / MBUS_BAUDRATE_PAYLOAD)

#define GAP_RAND_MASK 1023

#ifdef USART_CR1_UESM
#define CR1_HEADER \
  (USART_CR1_UE | USART_CR1_RXNEIE | USART_CR1_TE | USART_CR1_RE | \
   USART_CR1_UESM)
#else
#define CR1_HEADER \
  (USART_CR1_UE | USART_CR1_RXNEIE | USART_CR1_TE | USART_CR1_RE)
#endif

#define CR1_PAYLOAD_TX                                  \
  (USART_CR1_UE | USART_CR1_TCIE | USART_CR1_TE)
#define CR1_PAYLOAD_RX \
  (USART_CR1_UE | USART_CR1_RE | USART_CR1_TE)

#define MBUS_STATE_IDLE       0
#define MBUS_STATE_TX_HDR0    1
#define MBUS_STATE_TX_HDR1    2
#define MBUS_STATE_TX_PAUSE   3
#define MBUS_STATE_TX_PAYLOAD 4
#define MBUS_STATE_RX_HDR1    5
#define MBUS_STATE_RX_PAYLOAD 6
#define MBUS_STATE_GAP        7

typedef struct uart_mbus {
  mbus_netif_t um_mni;
  struct pbuf_queue txq;
  struct pbuf *rx;

  uint8_t state;
  uint8_t txq_len;
  uint8_t tx_attempts;

  uint32_t uart_reg_base;

  timer_t timer;

  gpio_t txe;

  uint32_t tx_header_timeout;
  uint32_t tx_header_bad;
  uint32_t tx_header_0;

  uint32_t rx_header_bytes;
  uint32_t rx_header_ok;
  uint32_t rx_header_timeout;
  uint32_t rx_header_crc;
  uint32_t rx_header_bad;
  uint32_t rx_header_noise;
  uint32_t rx_header_framing;
  uint32_t rx_nobufs;

  uint32_t rx_payload_bytes;
  uint32_t rx_payload_ok;
  uint32_t rx_payload_timeout;
  uint32_t rx_payload_noise;
  uint32_t rx_payload_framing;
  uint32_t rx_dma_error;

  stm32_dma_instance_t tx_dma;
  stm32_dma_instance_t rx_dma;

  uint32_t uart_bbr_header;
  uint32_t uart_bbr_payload;

  uint8_t rx_hdr[2];
  uint8_t tx_hdr[2];

} uart_mbus_t;



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

__attribute__((warn_unused_result))
static uint32_t
enter_gap(uart_mbus_t *um)
{
  gpio_set_output(um->txe, 0);
  reg_wr(um->uart_reg_base + USART_CR1, 0);
  reg_wr(um->uart_reg_base + USART_BRR, um->uart_bbr_header);
  reg_wr(um->uart_reg_base + USART_CR1, CR1_HEADER);
  um->state = MBUS_STATE_GAP;
  return 50 + (rand() & GAP_RAND_MASK);
}


__attribute__((warn_unused_result))
static uint32_t
transmit_payload(uart_mbus_t *um)
{
  pbuf_t *pb = STAILQ_FIRST(&um->txq);
  assert(pb != NULL);
  uint8_t *pkt = pbuf_data(pb, 0);

  um->state = MBUS_STATE_TX_PAYLOAD;

  reg_wr(um->uart_reg_base + USART_CR1, 0);
  reg_wr(um->uart_reg_base + USART_BRR, um->uart_bbr_payload);
  reg_wr(um->uart_reg_base + USART_CR1, CR1_PAYLOAD_TX);
  reg_set_bit(um->uart_reg_base + USART_CR3, 7);

  stm32_dma_set_nitems(um->tx_dma, pb->pb_pktlen - 1);
  stm32_dma_set_mem0(um->tx_dma, pkt + 1);

#ifdef USART_ICR
  reg_wr(um->uart_reg_base + USART_ICR, (1 << 6) | (1 << 7));
#endif

  stm32_dma_start(um->tx_dma);
  um->um_mni.mni_tx_packets++;
  return 250 + MBUS_PAYLOAD_BYTE_TIME * pb->pb_pktlen;
}

__attribute__((warn_unused_result))
static uint32_t
decode_header(uart_mbus_t *um)
{
  reg_wr(um->uart_reg_base + USART_CR1, 0);
  reg_wr(um->uart_reg_base + USART_BRR, um->uart_bbr_payload);
  reg_wr(um->uart_reg_base + USART_CR1, CR1_PAYLOAD_RX);

  if(likely(um->rx != NULL)) {

    if(likely(crc4(0, um->rx_hdr, 4)) == 0) {
      const uint8_t payload_len =
        (um->rx_hdr[0] >> 6) | ((um->rx_hdr[1] & 0xf) << 2);

      if(payload_len > 4) {
        um->rx_header_ok++;
        stm32_dma_set_nitems(um->rx_dma, payload_len);
        stm32_dma_start(um->rx_dma);
        reg_set_bit(um->uart_reg_base + USART_CR3, 6);


        um->state = MBUS_STATE_RX_PAYLOAD;

        const uint8_t dst = um->rx_hdr[0] & 0x3f;
        uint8_t *pkt = pbuf_data(um->rx, 0);
        pkt[0] = dst;
        const uint8_t len = payload_len + 1;
        um->rx->pb_pktlen = len;
        um->rx->pb_buflen = len;
        um->rx_payload_bytes += len;
        return 250 + MBUS_PAYLOAD_BYTE_TIME * len;
      }
      um->rx_header_bad++;

    } else {
      um->rx_header_crc++;
    }
  } else {
    um->rx_nobufs++;
  }

  reg_wr(um->uart_reg_base + USART_CR1, 0);
  reg_wr(um->uart_reg_base + USART_BRR, um->uart_bbr_header);
  reg_wr(um->uart_reg_base + USART_CR1, CR1_HEADER);

  wakelock_release();
  um->state = MBUS_STATE_IDLE;
  timer_disarm(&um->timer);
  return 0;
}

__attribute__((warn_unused_result))
static uint32_t
input_byte(uart_mbus_t *um, uint8_t b)
{
  switch(um->state) {
  case MBUS_STATE_IDLE:
    wakelock_acquire();
  case MBUS_STATE_GAP:
    um->rx_hdr[0] = b;
    um->state = MBUS_STATE_RX_HDR1;
    um->rx_header_bytes++;
    return 250;

  case MBUS_STATE_RX_HDR1:
    um->rx_hdr[1] = b;
    um->rx_header_bytes++;
    return decode_header(um);

  case MBUS_STATE_TX_HDR0:
    um->tx_header_0++;
    if(b != um->tx_hdr[0]) {
      um->tx_header_bad++;
      return enter_gap(um);
    }
    reg_wr(um->uart_reg_base + USART_TDR, um->tx_hdr[1]);
    um->state = MBUS_STATE_TX_HDR1;
    break;

  case MBUS_STATE_TX_HDR1:
    if(b != um->tx_hdr[1]) {
      um->tx_header_bad++;
      return enter_gap(um);
    }
    um->state = MBUS_STATE_TX_PAUSE;
    return 25;

  default:
    break;
  }
  return 0;
}

static void
splice(uart_mbus_t *um)
{
  um->txq_len--;
  um->tx_attempts = 0;
  pbuf_free(pbuf_splice(&um->txq));
}


__attribute__((warn_unused_result))
static uint32_t
start_tx(uart_mbus_t *um)
{
  um->tx_attempts++;

  if(um->tx_attempts == 10) {
    um->um_mni.mni_tx_fail++;
    splice(um);
    return 0;
  }

  if(mbus_uart_is_busy(um)) {
    return (um->tx_attempts * 100) + (rand() & GAP_RAND_MASK);
  }

  gpio_set_output(um->txe, 1);

  const pbuf_t *pb = STAILQ_FIRST(&um->txq);
  const uint8_t *pkt = pbuf_cdata(pb, 0);

  const uint8_t destination = pkt[0] & 0x3f;
  const uint8_t len = pb->pb_pktlen - 1;

  um->tx_hdr[0] = destination | (len << 6);
  um->tx_hdr[1] = len >> 2;
  uint8_t c = crc4(0, um->tx_hdr, 3);
  um->tx_hdr[1] |= c << 4;

  if(um->state == MBUS_STATE_IDLE)
    wakelock_acquire();

  um->state = MBUS_STATE_TX_HDR0;
  reg_wr(um->uart_reg_base + USART_TDR, um->tx_hdr[0]);
  return (um->tx_attempts * 100) + 150;
}

static void
mbus_uart_arm(uart_mbus_t *um, uint32_t delta)
{
  if(delta)
    timer_arm_abs(&um->timer, clock_get_irq_blocked() + delta);
}

static void
stm32_mbus_uart_irq(void *arg)
{
  uart_mbus_t *um = arg;
  uint32_t sr = reg_rd(um->uart_reg_base + USART_SR);

  uint32_t delta = 0;
#ifdef USART_ICR
  reg_wr(um->uart_reg_base + USART_ICR, sr);
#endif
  if(sr & (1 << 6)) {
    if(um->state == MBUS_STATE_TX_PAYLOAD) {
      stm32_dma_stop(um->tx_dma);
      reg_clr_bit(um->uart_reg_base + USART_CR3, 7);

      splice(um);
      delta = enter_gap(um);
    }
  }

  if(sr & (1 << 5)) {
    const uint8_t c = reg_rd(um->uart_reg_base + USART_RDR);
    int err = sr & 6;
    if(!err) {
      delta = input_byte(um, c);
    } else {
      if(err & 2)
        um->rx_header_framing++;
      if(err & 4)
        um->rx_header_noise++;
    }
  }

  mbus_uart_arm(um, delta);
}


static int
rx_payload_read_sr(uart_mbus_t *um)
{
  uint32_t sr = reg_rd(um->uart_reg_base + USART_SR);
#ifdef USART_ICR
  reg_wr(um->uart_reg_base + USART_ICR, sr);
#else
  reg_rd(um->uart_reg_base + USART_RDR);
#endif
  if(sr & 2)
    um->rx_payload_framing++;
  if(sr & 4)
    um->rx_payload_noise++;

  return sr & 6;
}

__attribute__((warn_unused_result))
static uint32_t
rx_payload_timeout(uart_mbus_t *um)
{
  stm32_dma_stop(um->rx_dma);
  reg_clr_bit(um->uart_reg_base + USART_CR3, 6);

  um->rx_payload_timeout++;
  rx_payload_read_sr(um);
  return enter_gap(um);
}

static void
rx_complete(stm32_dma_instance_t instance, void *arg, error_t err)
{
  uart_mbus_t *um = arg;

  if(!err) {

    if(um->state == MBUS_STATE_RX_PAYLOAD) {

      if(!rx_payload_read_sr(um))
        um->rx_payload_ok++;
      int q = irq_forbid(IRQ_LEVEL_NET);
      pbuf_t *nxt = pbuf_make_irq_blocked(0, 0);
      if(nxt) {
        STAILQ_INSERT_TAIL(&um->um_mni.mni_ni.ni_rx_queue, um->rx, pb_link);
        netif_wakeup(&um->um_mni.mni_ni);

        um->rx = nxt;
        stm32_dma_set_mem0(um->rx_dma, um->rx->pb_data + 1);
        memset(um->rx->pb_data, 0xaa, 64);
      }
      irq_permit(q);
    }

  } else {
    um->rx_dma_error++;
  }

  stm32_dma_stop(um->rx_dma);
  reg_clr_bit(um->uart_reg_base + USART_CR3, 6);

  mbus_uart_arm(um, enter_gap(um));
}


static pbuf_t *
mbus_uart_output(struct mbus_netif *mni, pbuf_t *pb)
{
  pb = pbuf_pullup(pb, pb->pb_pktlen);
  if(pb == NULL)
    return NULL;

  uart_mbus_t *um = (uart_mbus_t *)mni;
  int q = irq_forbid(IRQ_LEVEL_CLOCK);
  if(um->txq_len < 5) {
    um->txq_len++;
    STAILQ_INSERT_TAIL(&um->txq, pb, pb_link);
    pb = NULL;
    if(um->state == MBUS_STATE_IDLE) {
      mbus_uart_arm(um, start_tx(um));
    }
  } else {
    mni->mni_tx_qdrops++;
  }
  irq_permit(q);
  return pb;
}



static void
timer_fire(void *opaque, uint64_t now)
{
  uart_mbus_t *um = (uart_mbus_t *)opaque;
  int delta = 0;

  switch(um->state) {
  case MBUS_STATE_GAP:
    um->state = MBUS_STATE_IDLE;
    wakelock_release();

  case MBUS_STATE_IDLE:
    if(um->txq_len)
      delta = start_tx(um);
    break;
  case MBUS_STATE_TX_HDR0:
  case MBUS_STATE_TX_HDR1:
    um->tx_header_timeout++;
    delta = enter_gap(um);
    break;
  case MBUS_STATE_RX_HDR1:
    um->rx_header_timeout++;
    delta = enter_gap(um);
    break;
  case MBUS_STATE_RX_PAYLOAD:
    delta = rx_payload_timeout(um);
    break;
  case MBUS_STATE_TX_PAUSE:
    delta = transmit_payload(um);
    break;
  default:
    gpio_set_output(um->txe, 0);
    delta = 0;
    break;
  }

  mbus_uart_arm(um, delta);
}


static void
mbus_uart_print_info(struct device *dev, struct stream *st)
{
  uart_mbus_t *um = (uart_mbus_t *)dev;
  stprintf(st, "\tTX Header:\n");
  stprintf(st, "\t\tTimeout:%u  Bad:%u  state0:%u\n",
           um->tx_header_timeout, um->tx_header_bad,
           um->tx_header_0);

  stprintf(st, "\tRX Header:\n");
  stprintf(st, "\t\tBytes:%u  OK:%u  Timeout:%u  ",
           um->rx_header_bytes,
           um->rx_header_ok,
           um->rx_header_timeout);

  stprintf(st, "Noise:%u  Framing:%u  CRC:%u  Bad:%u  \n",
           um->rx_header_noise,
           um->rx_header_framing,
           um->rx_header_crc,
           um->rx_header_bad);

  stprintf(st, "\tRX Payload:\n");
  stprintf(st, "\t\tBytes:%u  OK:%u  Timeout:%u  ",
           um->rx_payload_bytes,
           um->rx_payload_ok,
           um->rx_payload_timeout);

  stprintf(st, "Noise:%u  Framing:%u  DMAERR:%u\n",
           um->rx_payload_noise,
           um->rx_payload_framing,
           um->rx_dma_error);

  mbus_print_info(&um->um_mni, st);
}


static void
mbus_uart_power_state(struct device *dev, device_power_state_t state)
{
  uart_mbus_t *um = (uart_mbus_t *)dev;
  if(state == DEVICE_POWER_STATE_SUSPEND) {
    // Make sure we are silent on bus when suspended
    gpio_set_output(um->txe, 0);
  }
}

static const device_class_t mbus_uart_device_class = {
  .dc_print_info = mbus_uart_print_info,
  .dc_power_state = mbus_uart_power_state,
};


static void
buffers_avail(netif_t *ni)
{
  uart_mbus_t *um = (uart_mbus_t *)ni;
  if(um->rx == NULL) {
    um->rx = pbuf_make(0, 0);
    if(um->rx != NULL) {
      stm32_dma_set_mem0(um->rx_dma, um->rx->pb_data + 1);
    }
  }
}


static uint32_t
bbr(uint32_t freq, uint32_t baudrate)
{
  return (freq + baudrate - 1) / baudrate;
}


static void
stm32_mbus_uart_create(uint32_t uart_reg_base,
                       int clkid, int uart_irq,
                       uint32_t tx_dma_resouce_id,
                       uint32_t rx_dma_resouce_id,
                       gpio_t txe, int flags, uint32_t freq)
{
  clk_enable(clkid);

  uart_mbus_t *um = calloc(1, sizeof(uart_mbus_t));
  um->uart_reg_base = uart_reg_base;

  um->uart_bbr_header = bbr(freq, MBUS_BAUDRATE_HEADER);
  um->uart_bbr_payload = bbr(freq, MBUS_BAUDRATE_PAYLOAD);

  // TX-DMA

  um->tx_dma = stm32_dma_alloc(tx_dma_resouce_id, "mbus-tx");

  stm32_dma_config(um->tx_dma,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_PRIO_LOW,
                   STM32_DMA_8BIT,
                   STM32_DMA_8BIT,
                   STM32_DMA_INCREMENT,
                   STM32_DMA_FIXED,
                   STM32_DMA_SINGLE,
                   STM32_DMA_M_TO_P);

  stm32_dma_set_paddr(um->tx_dma, uart_reg_base + USART_TDR);

  // RX-DMA

  um->rx_dma = stm32_dma_alloc(rx_dma_resouce_id, "mbus-rx");

  stm32_dma_set_callback(um->rx_dma, rx_complete, um, IRQ_LEVEL_CLOCK);

  stm32_dma_config(um->rx_dma,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_PRIO_HIGH,
                   STM32_DMA_8BIT,
                   STM32_DMA_8BIT,
                   STM32_DMA_INCREMENT,
                   STM32_DMA_FIXED,
                   STM32_DMA_SINGLE,
                   STM32_DMA_P_TO_M);

  stm32_dma_set_paddr(um->rx_dma, uart_reg_base + USART_RDR);

#if defined(USART_CR2) && defined(USART_CR3)
  if(flags & UART_WAKEUP) {
    reg_wr(um->uart_reg_base + USART_CR3, 0b111 << 20);
  }
#endif

  reg_wr(um->uart_reg_base + USART_BRR, um->uart_bbr_header);
  reg_wr(um->uart_reg_base + USART_CR1, CR1_HEADER);

  um->txe = txe;
  um->um_mni.mni_output = mbus_uart_output;
  um->um_mni.mni_ni.ni_buffers_avail = buffers_avail;

  STAILQ_INIT(&um->txq);

  um->um_mni.mni_ni.ni_mtu = 64;

  um->timer.t_cb = timer_fire;
  um->timer.t_opaque = um;

  mbus_netif_attach(&um->um_mni, "uartmbus", &mbus_uart_device_class);

  irq_enable_fn_arg(uart_irq, IRQ_LEVEL_CLOCK, stm32_mbus_uart_irq, um);
}
