#include "nrf52_mbus_uart.h"

#include "nrf52_reg.h"
#include "nrf52_rng.h"
#include "nrf52_timer.h"

#include <mios/io.h>
#include <net/mbus/mbus.h>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include "util/crc4.h"

#include "irq.h"

#define MBUS_BAUDRATE_HEADER  115200
#define MBUS_BAUDRATE_PAYLOAD 1000000

#define MBUS_PAYLOAD_BYTE_TIME (10 * 1000000 / MBUS_BAUDRATE_PAYLOAD)

#define MBUS_UART_TIMER_BASE TIMER2_BASE
#define MBUS_UART_TIMER_IRQ  TIMER2_IRQ

#define UARTE_BASE 0x40002000

#define UARTE_STARTRX  (UARTE_BASE + 0x000)
#define UARTE_STOPRX   (UARTE_BASE + 0x004)
#define UARTE_STARTTX  (UARTE_BASE + 0x008)
#define UARTE_STOPTX   (UARTE_BASE + 0x00c)
#define UARTE_FLUSHRX  (UARTE_BASE + 0x02c)

#define UARTE_EVENTS_CTS       (UARTE_BASE + 0x100)
#define UARTE_EVENTS_NCTS      (UARTE_BASE + 0x104)
#define UARTE_EVENTS_RXDRDY    (UARTE_BASE + 0x108)
#define UARTE_EVENTS_ENDRX     (UARTE_BASE + 0x110)
#define UARTE_EVENTS_TXDRDY    (UARTE_BASE + 0x11c)
#define UARTE_EVENTS_ENDTX     (UARTE_BASE + 0x120)
#define UARTE_EVENTS_ERROR     (UARTE_BASE + 0x124)
#define UARTE_EVENTS_RXTO      (UARTE_BASE + 0x144)
#define UARTE_EVENTS_RXSTARTED (UARTE_BASE + 0x14c)
#define UARTE_EVENTS_TXSTARTED (UARTE_BASE + 0x150)
#define UARTE_EVENTS_TXSTOPPED (UARTE_BASE + 0x158)

#define UARTE_INTEN           (UARTE_BASE + 0x300)
#define UARTE_INTENSET        (UARTE_BASE + 0x304)
#define UARTE_INTENCLR        (UARTE_BASE + 0x308)
#define UARTE_ENABLE          (UARTE_BASE + 0x500)
#define UARTE_PSELTXD         (UARTE_BASE + 0x50c)
#define UARTE_PSELRXD         (UARTE_BASE + 0x514)
#define UARTE_BAUDRATE        (UARTE_BASE + 0x524)

#define UARTE_RXD_PTR         (UARTE_BASE + 0x534)
#define UARTE_RXD_MAXCNT      (UARTE_BASE + 0x538)
#define UARTE_RXD_AMOUNT      (UARTE_BASE + 0x53c)
#define UARTE_TXD_PTR         (UARTE_BASE + 0x544)
#define UARTE_TXD_MAXCNT      (UARTE_BASE + 0x548)
#define UARTE_TXD_AMOUNT      (UARTE_BASE + 0x54c)




#define MBUS_STATE_IDLE       0
#define MBUS_STATE_TX_HDR0    1
#define MBUS_STATE_TX_HDR1    2
#define MBUS_STATE_TX_PAUSE   3
#define MBUS_STATE_TX_PAYLOAD 4
#define MBUS_STATE_RX_HDR1    5
#define MBUS_STATE_RX_PAYLOAD 6
#define MBUS_STATE_GAP        7
#define MBUS_STATE_DISABLED   8

typedef struct uart_mbus {
  mbus_netif_t um_mni;
  struct pbuf_queue txq;
  struct pbuf *rx;

  uint8_t state;
  uint8_t txq_len;
  uint8_t tx_attempts;
  uint8_t disabled;

  uint32_t uart_reg_base;

  gpio_t txe_pin;
  gpio_t rxe_pin;
  gpio_t tx_pin;
  gpio_t rx_pin;

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

  uint32_t uart_bbr_header;
  uint32_t uart_bbr_payload;

  uint8_t rx_hdr[2];
  uint8_t tx_hdr[2];

} uart_mbus_t;

static void
mbus_uart_enable(uart_mbus_t *um)
{
  gpio_conf_output(um->txe_pin, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_output(um->rxe_pin, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  reg_wr(UARTE_ENABLE, 8);
  reg_wr(UARTE_PSELTXD, um->tx_pin);
  reg_wr(UARTE_PSELRXD, um->rx_pin);
}


static void
mbus_uart_disable(uart_mbus_t *um)
{
  reg_wr(UARTE_PSELTXD, 1 << 31);
  reg_wr(UARTE_PSELRXD, 1 << 31);
  gpio_disconnect(um->txe_pin);
  gpio_disconnect(um->rxe_pin);

  reg_wr(UARTE_STOPRX, 1);
  reg_wr(UARTE_STOPTX, 1);
  reg_wr(UARTE_INTEN, 0);
  reg_wr(UARTE_ENABLE, 0);

  reg_wr(MBUS_UART_TIMER_BASE + TIMER_TASKS_STOP, 1);
  um->state = MBUS_STATE_DISABLED;
}


static void
disarm_timer()
{
  reg_wr(MBUS_UART_TIMER_BASE + TIMER_TASKS_CLEAR, 1);
}

static void
arm_timer(uart_mbus_t *um, uint32_t usecs)
{
  assert(um->state != MBUS_STATE_TX_PAYLOAD);
  reg_wr(MBUS_UART_TIMER_BASE + TIMER_TASKS_CLEAR, 1);
  reg_wr(MBUS_UART_TIMER_BASE + TIMER_CC(0), usecs);
  reg_wr(MBUS_UART_TIMER_BASE + TIMER_TASKS_START, 1);
}

static void
splice(uart_mbus_t *um)
{
  um->txq_len--;
  um->tx_attempts = 0;
  pbuf_free(pbuf_splice(&um->txq));
}


static void
enter_gap(uart_mbus_t *um)
{
  gpio_set_output(um->txe_pin, 0);
  gpio_set_output(um->rxe_pin, 0);

  um->state = MBUS_STATE_GAP;
  int delta = 50 + (reg_rd(RNG_VALUE) << 2);

  reg_wr(UARTE_BAUDRATE, 0x1d60000);
  reg_wr(UARTE_RXD_PTR, (intptr_t)um->rx_hdr);
  reg_wr(UARTE_RXD_MAXCNT, 1);
  reg_wr(UARTE_STARTRX, 1);
  reg_wr(UARTE_INTEN, (1 << 4)); // ENDRX

  arm_timer(um, delta);
}


static void
start_tx(uart_mbus_t *um)
{
  um->tx_attempts++;

  if(um->tx_attempts == 10) {
    um->um_mni.mni_tx_fail++;
    splice(um);
    return;
  }

  gpio_set_output(um->txe_pin, 1);

  const pbuf_t *pb = STAILQ_FIRST(&um->txq);
  const uint8_t *pkt = pbuf_cdata(pb, 0);

  const uint8_t destination = pkt[0] & 0x3f;
  const uint8_t len = pb->pb_pktlen - 1;

  um->tx_hdr[0] = destination | (len << 6);
  um->tx_hdr[1] = len >> 2;
  uint8_t c = crc4(0, um->tx_hdr, 3);
  um->tx_hdr[1] |= c << 4;

  um->state = MBUS_STATE_TX_HDR0;

  reg_wr(UARTE_TXD_PTR, (intptr_t)um->tx_hdr);
  reg_wr(UARTE_TXD_MAXCNT, 1);
  reg_wr(UARTE_STARTTX, 1);

  arm_timer(um, um->tx_attempts * 100 + 150);
}


static void
transmit_payload(uart_mbus_t *um)
{
  reg_wr(UARTE_BAUDRATE, 0x10000000);

  pbuf_t *pb = STAILQ_FIRST(&um->txq);
  assert(pb != NULL);
  uint8_t *pkt = pbuf_data(pb, 0);

  gpio_set_output(um->rxe_pin, 1);

  reg_wr(UARTE_TXD_PTR, (intptr_t)pkt + 1);
  reg_wr(UARTE_TXD_MAXCNT, pb->pb_pktlen - 1);
  reg_wr(UARTE_STARTTX, 1);
  um->state = MBUS_STATE_TX_PAYLOAD;
  reg_wr(UARTE_INTEN, (1 << 8)); // ENDTX
}



static void
decode_header(uart_mbus_t *um)
{
  if(likely(um->rx != NULL)) {
    if(likely(crc4(0, um->rx_hdr, 4)) == 0) {
      const uint8_t payload_len =
        (um->rx_hdr[0] >> 6) | ((um->rx_hdr[1] & 0xf) << 2);

      if(payload_len > 4) {
        um->rx_header_ok++;

        const uint8_t dst = um->rx_hdr[0] & 0x3f;
        uint8_t *pkt = pbuf_data(um->rx, 0);
        pkt[0] = dst;

        reg_wr(UARTE_BAUDRATE, 0x10000000);
        reg_wr(UARTE_RXD_PTR, (intptr_t)pkt + 1);
        reg_wr(UARTE_RXD_MAXCNT, payload_len);
        reg_wr(UARTE_STARTRX, 1);
        reg_wr(UARTE_INTEN, (1 << 4)); // ENDRX
        um->state = MBUS_STATE_RX_PAYLOAD;

        const uint8_t len = payload_len + 1;
        um->rx->pb_pktlen = len;
        um->rx->pb_buflen = len;
        um->rx_payload_bytes += len;
        arm_timer(um, 250 + MBUS_PAYLOAD_BYTE_TIME * len);
        return;
      }
      um->rx_header_bad++;
    } else {
      um->rx_header_crc++;
    }
  } else {
    um->rx_nobufs++;
  }
  um->state = MBUS_STATE_IDLE;
  disarm_timer();

  if(um->disabled) {
    mbus_uart_disable(um);
  }
}



static void
rx_done(uart_mbus_t *um)
{
  pbuf_t *nxt = pbuf_make_irq_blocked(0, 0);
  if(nxt) {
    STAILQ_INSERT_TAIL(&um->um_mni.mni_ni.ni_rx_queue, um->rx, pb_link);
    netif_wakeup(&um->um_mni.mni_ni);
    um->rx = nxt;
  }

  enter_gap(um);
}

static void
nrf52_mbus_uart_irq(void *arg)
{
  uart_mbus_t *um = arg;

  if(reg_rd(UARTE_EVENTS_ENDTX)) {
    reg_wr(UARTE_EVENTS_ENDTX, 0);

    if(um->state == MBUS_STATE_TX_PAYLOAD) {
      splice(um);
      enter_gap(um);
    }
    return;
  }

  if(reg_rd(UARTE_EVENTS_ENDRX)) {
    reg_wr(UARTE_EVENTS_ENDRX, 0);

    switch(um->state) {
    case MBUS_STATE_TX_HDR0:
      um->tx_header_0++;

      if(um->rx_hdr[0] != um->tx_hdr[0]) {
        um->tx_header_bad++;
        enter_gap(um);
        return;
      }

      reg_wr(UARTE_RXD_PTR, (intptr_t)um->rx_hdr + 1);
      reg_wr(UARTE_RXD_MAXCNT, 1);
      reg_wr(UARTE_STARTRX, 1);

      reg_wr(UARTE_TXD_PTR, (intptr_t)um->tx_hdr + 1);
      reg_wr(UARTE_TXD_MAXCNT, 1);
      reg_wr(UARTE_STARTTX, 1);
      um->state = MBUS_STATE_TX_HDR1;
      break;

    case MBUS_STATE_TX_HDR1:
      if(um->rx_hdr[1] != um->tx_hdr[1]) {
        um->tx_header_bad++;
        enter_gap(um);
        return;
      }
      reg_wr(UARTE_STOPRX, 1);
      um->state = MBUS_STATE_TX_PAUSE;
      arm_timer(um, 25);
      break;

    case MBUS_STATE_IDLE:
    case MBUS_STATE_GAP:

      reg_wr(UARTE_RXD_PTR, (intptr_t)um->rx_hdr + 1);
      reg_wr(UARTE_RXD_MAXCNT, 1);
      reg_wr(UARTE_STARTRX, 1);

      um->state = MBUS_STATE_RX_HDR1;
      arm_timer(um, 250);
      break;

    case MBUS_STATE_RX_HDR1:
      um->rx_header_bytes++;
      return decode_header(um);

    case MBUS_STATE_RX_PAYLOAD:
      rx_done(um);
      return;

    default:
      break;
    }

  } else {
    panic("otherirq");
  }
}




static void
nrf52_mbus_uart_timer_irq(void *arg)
{
  uart_mbus_t *um = arg;

  if(reg_rd(MBUS_UART_TIMER_BASE + TIMER_EVENTS_COMPARE(0))) {
    reg_wr(MBUS_UART_TIMER_BASE + TIMER_EVENTS_COMPARE(0), 0);

    switch(um->state) {

    case MBUS_STATE_GAP:
      um->state = MBUS_STATE_IDLE;
      // FALLTHRU
    case MBUS_STATE_IDLE:
      if(um->txq_len) {
        start_tx(um);
      } else {

        if(um->disabled) {
          mbus_uart_disable(um);
        }
      }
      break;
    case MBUS_STATE_TX_HDR0:
    case MBUS_STATE_TX_HDR1:
      um->tx_header_timeout++;
      enter_gap(um);
      break;
    case MBUS_STATE_RX_HDR1:
      um->rx_header_timeout++;
      enter_gap(um);
      break;
    case MBUS_STATE_RX_PAYLOAD:
      um->rx_payload_timeout++;
      reg_wr(UARTE_STOPRX, 1);
      enter_gap(um);
      break;
    case MBUS_STATE_TX_PAUSE:
      transmit_payload(um);
      break;
    case MBUS_STATE_TX_PAYLOAD:
      break;
    case MBUS_STATE_DISABLED:
      break;
    default:
      panic("Bad timer state");
    }
  } else {
    panic("unknown timer irq");
  }
}


static pbuf_t *
mbus_uart_output(struct mbus_netif *mni, pbuf_t *pb)
{
  if(pbuf_pullup(pb, pb->pb_pktlen)) {
    panic("pullup failed");
  }
  uart_mbus_t *um = (uart_mbus_t *)mni;
  int q = irq_forbid(IRQ_LEVEL_NET);
  if(um->txq_len < 5) {
    um->txq_len++;
    STAILQ_INSERT_TAIL(&um->txq, pb, pb_link);
    pb = NULL;
    if(um->state == MBUS_STATE_IDLE) {
      start_tx(um);
    }
  } else {
    mni->mni_tx_qdrops++;
  }
  irq_permit(q);
  return pb;
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
  int q = irq_forbid(IRQ_LEVEL_NET);

  switch(state) {
  case DEVICE_POWER_STATE_SUSPEND:
    if(um->state == MBUS_STATE_IDLE) {
      mbus_uart_disable(um);
    }
    um->disabled = 1;
    break;
  case DEVICE_POWER_STATE_RESUME:
    um->disabled = 0;
    mbus_uart_enable(um);
    enter_gap(um);
    break;
  }

  irq_permit(q);
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
  }
}


void
nrf52_mbus_uart_init(gpio_t tx, gpio_t rx, gpio_t txe, gpio_t rxe)
{
  uart_mbus_t *um = calloc(1, sizeof(uart_mbus_t));
  um->tx_pin = tx;
  um->rx_pin = rx;
  um->txe_pin = txe;
  um->rxe_pin = rxe;

  mbus_uart_enable(um);

  enter_gap(um);

  um->um_mni.mni_output = mbus_uart_output;
  um->um_mni.mni_ni.ni_buffers_avail = buffers_avail;
  STAILQ_INIT(&um->txq);

  mbus_netif_attach(&um->um_mni, "uartmbus", &mbus_uart_device_class);

  irq_enable_fn_arg(2, IRQ_LEVEL_NET, nrf52_mbus_uart_irq, um);

  reg_wr(MBUS_UART_TIMER_BASE + TIMER_BITMODE, 3);        // 32 bit width
  reg_wr(MBUS_UART_TIMER_BASE + TIMER_INTENSET, 1 << 16); // Compare0 -> IRQ

  irq_enable_fn_arg(MBUS_UART_TIMER_IRQ, IRQ_LEVEL_NET,
                    nrf52_mbus_uart_timer_irq, um);
}
