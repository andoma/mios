#include <mios/stream.h>
#include <mios/fifo.h>
#include <mios/task.h>

#include <stdio.h>
#include <stdlib.h>

#include "irq.h"

#include "drivers/uart_16550.h"
#include "tegra234-hsp.h"

// Tegra Combined UART

#define TCU_CHANNEL_SPE    0xe0
#define TCU_CHANNEL_CCPLEX 0xe1
#define TCU_CHANNEL_BPMP   0xe2
#define TCU_CHANNEL_SCE    0xe3
#define TCU_CHANNEL_TZ     0xe4
#define TCU_CHANNEL_RCE    0xe5

#define NUM_MBOX_STREAMS 5

typedef struct tcu {

  stream_t spe;          // System console for SPE (must be first in struct)

  stream_t *ext_uart;    // External UART

  uint8_t tx_channel;
  mutex_t tx_mutex;

  mutex_t spe_rx_mutex;
  cond_t spe_rx_cond;
  FIFO_DECL(spe_rx_fifo, 64);

  stream_t *mbox_streams[NUM_MBOX_STREAMS];

} tcu_t;

static const uint8_t mbox_stream_to_channel_id[NUM_MBOX_STREAMS] = {
  TCU_CHANNEL_CCPLEX,
  TCU_CHANNEL_SCE,
  TCU_CHANNEL_RCE,
  TCU_CHANNEL_BPMP,
  TCU_CHANNEL_TZ,
};

static void
tcu_tx(tcu_t *tcu, const void *buf, size_t len, uint8_t channel)
{
  mutex_lock(&tcu->tx_mutex);
  if(channel != tcu->tx_channel) {
    uint8_t cmd[2] = {0xff, channel};
    tcu->tx_channel = channel;
    stream_write(tcu->ext_uart, cmd, 2, 0);
  }
  stream_write(tcu->ext_uart, buf, len, 0);
  mutex_unlock(&tcu->tx_mutex);
}



__attribute__((noreturn))
static void *
tcu_rx_thread(void *arg)
{
  tcu_t *tcu = arg;

  uint8_t channel = 0;
  int esc = 0;

  uint8_t buf[16];

  while(1) {
    int r = stream_read(tcu->ext_uart, buf, sizeof(buf), 1);

    for(size_t i = 0; i < r; i++) {
      if(esc) {
        channel = buf[i];
        esc = 0;
        continue;
      }
      if(buf[i] == 0xff) {
        esc = 1;
        continue;
      }

      if(channel == TCU_CHANNEL_SPE) {
        mutex_lock(&tcu->spe_rx_mutex);

        if(!fifo_is_full(&tcu->spe_rx_fifo)) {
          fifo_wr(&tcu->spe_rx_fifo, buf[i]);
        }
        cond_signal(&tcu->spe_rx_cond);
        mutex_unlock(&tcu->spe_rx_mutex);
      } else if(channel == TCU_CHANNEL_CCPLEX) {
        stream_write(tcu->mbox_streams[0], buf + i, 1, 0);
      } else {
        printf("Got input on channel 0x%x: 0x%x\n", channel, buf[i]);
      }
    }
  }
}


__attribute__((noreturn))
static void *
tcu_tx_thread(void *arg)
{
  tcu_t *tcu = arg;

  pollset_t ps[NUM_MBOX_STREAMS];

  for(size_t i = 0; i < NUM_MBOX_STREAMS; i++) {
    ps[i].type = POLL_STREAM_READ;
    ps[i].obj = tcu->mbox_streams[i];
  }

  uint8_t buf[16];

  while(1) {
    int which = poll(ps, NUM_MBOX_STREAMS, NULL, INT64_MAX);
    int r = stream_read(tcu->mbox_streams[which], buf, sizeof(buf), 0);
    if(r)
      tcu_tx(tcu, buf, r, mbox_stream_to_channel_id[which]);
  }
}


static ssize_t
spe_console_write(struct stream *s, const void *buf,
                  size_t size, int flags)
{
  tcu_t *tcu = (tcu_t *)s;

  if(!can_sleep()) {

    if(tcu->tx_channel != TCU_CHANNEL_SPE) {
      uint8_t cmd[2] = {0xff, TCU_CHANNEL_SPE};
      tcu->tx_channel = TCU_CHANNEL_SPE;
      stream_write(tcu->ext_uart, cmd, 2, 0);
    }
    return stream_write(tcu->ext_uart, buf, size, flags);
  }

  tcu_tx(tcu, buf, size, TCU_CHANNEL_SPE);
  return size;
}


static ssize_t
spe_console_read(struct stream *s, void *buf,
                 size_t size, size_t required)
{
  tcu_t *tcu = (tcu_t *)s;

  if(!can_sleep()) {
    while(1) {
      // FIX THIS
    }
  }

  uint8_t *u8 = buf;

  size_t written = 0;
  mutex_lock(&tcu->spe_rx_mutex);

  while(written < size) {

    if(fifo_is_empty(&tcu->spe_rx_fifo)) {
      if(written >= required)
        break;
      cond_wait(&tcu->spe_rx_cond, &tcu->spe_rx_mutex);
      continue;
    }
    *u8++ = fifo_rd(&tcu->spe_rx_fifo);
    written++;
  }
  mutex_unlock(&tcu->spe_rx_mutex);
  return written;
}


 static const stream_vtable_t spe_console_vtable = {
  .write = spe_console_write,
  .read = spe_console_read,
};

static void  __attribute__((constructor(200)))
tcu_init(void)
{
  tcu_t *tcu = calloc(1, sizeof(tcu_t));

  tcu->ext_uart = uart_16550_create(0x0c280000, 22); // UART-C
  tcu->spe.vtable = &spe_console_vtable;
  stdio = &tcu->spe;

  tcu->mbox_streams[0] = hsp_mbox_stream(1,        // CCPLEX
                                         NV_ADDRESS_MAP_TOP0_HSP_BASE, 0);
  tcu->mbox_streams[1] = hsp_mbox_stream(3, 0, 0); // DCE
  tcu->mbox_streams[2] = hsp_mbox_stream(5, 0, 0); // RCE
  tcu->mbox_streams[3] = hsp_mbox_stream(6, 0, 0); // BPMP
  tcu->mbox_streams[4] = hsp_mbox_stream(7, 0, 0); // TZ

  thread_create(tcu_rx_thread, tcu, 512, "tcu_rx", TASK_DETACHED, 4);
  thread_create(tcu_tx_thread, tcu, 512, "tcu_tx", TASK_DETACHED, 4);
}
