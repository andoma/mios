#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include <mios/suspend.h>
#include <mios/task.h>
#include <mios/fifo.h>
#include <mios/timer.h>

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

  timer_t timer;
  task_t softirq;

  uint8_t state;
  uint8_t txoff;
  uint8_t prio;
  uint8_t flags;
  uint8_t tx_state;
  uint8_t tx_queue_len;
  uint8_t tx_attempts;

  gpio_t txe;

  FIFO_DECL(rxfifo, 8);

  uint32_t rx_fifo_full;
  uint32_t rx_framing_error;
  uint32_t rx_noise_error;

} uart_mbus_t;

static void mbus_uart_tx_byte(uart_mbus_t *um, uint8_t byte);

static int mbus_uart_is_busy(uart_mbus_t *um);



static void
start_timer(uart_mbus_t *um)
{
  int timeout = 1000 + (rand() & 0x1ff);
  timer_arm_abs(&um->timer, clock_get_irq_blocked() + timeout);
}


static uint8_t
tx_byte(uart_mbus_t *um, uint8_t xor)
{
  pbuf_t *pb = STAILQ_FIRST(&um->tx_queue);
  if(um->txoff == pb->pb_pktlen) {
    mbus_uart_tx_byte(um, 0x7e);
    return MBUS_STATE_TX_EOF;
  }

  const uint8_t *d = pbuf_cdata(pb, um->txoff);
  const uint8_t byte = *d ^ xor;

  if(byte == 0x7d || byte == 0x7e) {
    mbus_uart_tx_byte(um, 0x7d);
    return MBUS_STATE_TX_FLAG;
  }

  mbus_uart_tx_byte(um, byte);
  um->txoff++;
  return xor ? MBUS_STATE_TX_ESC : MBUS_STATE_TX_DAT;
}


static void
splice(uart_mbus_t *um)
{
  um->tx_queue_len--;
  um->tx_attempts = 0;
  pbuf_free(pbuf_splice(&um->tx_queue));
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

  if(mbus_uart_is_busy(um))
    return;

  gpio_set_output(um->txe, 1);
  um->state = MBUS_STATE_TX_SOF;
  um->txoff = 0;
  mbus_uart_tx_byte(um, 0x7e);
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
rxbyte(uart_mbus_t *um, uint8_t c)
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

        int q = irq_forbid(IRQ_LEVEL_NET);
        STAILQ_INSERT_TAIL(&um->um_mni.mni_ni.ni_rx_queue, um->rx_pbuf,
                           pb_link);
        netif_wakeup(&um->um_mni.mni_ni);
        irq_permit(q);
      }
      um->rx_pbuf = pbuf_make(0, 0);
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
softirq(task_t *t)
{
  uart_mbus_t *um = (void *)t - offsetof(uart_mbus_t, softirq);

  int q = irq_forbid(IRQ_LEVEL_CONSOLE);

  if(fifo_avail(&um->rxfifo)) {
    uint8_t byte = fifo_rd(&um->rxfifo);
    irq_permit(q);
    rxbyte(um, byte);
  } else {
    irq_permit(q);
  }
  q = irq_forbid(IRQ_LEVEL_CLOCK);
  start_timer(um);
  irq_permit(q);
}


static pbuf_t *
output_hd(struct mbus_netif *mni, pbuf_t *pb)
{
  uart_mbus_t *um = (uart_mbus_t *)mni;
  int q = irq_forbid(IRQ_LEVEL_CLOCK);
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


static void
timer_fire(void *arg, uint64_t now)
{
  uart_mbus_t *um = arg;

  if(STAILQ_FIRST(&um->tx_queue) != NULL) {
    start_tx_hd(um);
  } else {
    um->state = MBUS_STATE_IDLE;
    gpio_set_output(um->txe, 0);
    if(um->flags & UART_WAKEUP)
      wakelock_release();
  }
}


static void
mbus_uart_print_info(struct device *dev, struct stream *st)
{
  uart_mbus_t *um = (uart_mbus_t *)dev;
  stprintf(st, "\tByte interface:\n");
  stprintf(st, "\t\t%u Fifo overrun  %u Framing  %u Noise\n",
           um->rx_fifo_full,
           um->rx_framing_error, um->rx_noise_error);
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
mbus_uart_init_common(uart_mbus_t *um, gpio_t txe, uint8_t local_addr,
                      uint8_t prio, int flags)
{
  um->txe = txe;

  um->um_mni.mni_output = output_hd;

  STAILQ_INIT(&um->tx_queue);
  um->state = MBUS_STATE_GAP;
  um->prio = prio;
  um->flags = flags;

  um->um_mni.mni_ni.ni_mtu = 32;

  um->timer.t_cb = timer_fire;
  um->timer.t_opaque = um;

  um->softirq.t_run = softirq;
  um->softirq.t_prio = 6;

  mbus_netif_attach(&um->um_mni, "uartmbus", &mbus_uart_device_class,
                    local_addr, MBUS_NETIF_ENABLE_PCS);
}
