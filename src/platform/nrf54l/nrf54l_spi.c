#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/param.h>
#include <sys/uio.h>

#include <mios/io.h>
#include <mios/task.h>
#include <mios/device.h>

#include "irq.h"
#include "nrf54l_reg.h"
#include "nrf54l_spi.h"

// SPIM register map (differs from the nRF52 SPIM: EasyDMA moved to a DMA.*
// block at 0x700, PSEL.* at 0x600, a PRESCALER divisor instead of FREQUENCY).
#define SPIM_TASKS_START   0x000
#define SPIM_EVENTS_END    0x108  // RXD and TXD buffers both reached
#define SPIM_INTENSET      0x304
#define SPIM_INTENSET_END  (1 << 2)
#define SPIM_ENABLE        0x500  // 7 = enable, 0 = disable
#define SPIM_PRESCALER     0x52c  // SCK = base_clock / DIVISOR, DIVISOR 2..126
#define SPIM_CONFIG        0x554  // bit0 ORDER(0=MSB), bit1 CPHA, bit2 CPOL
#define SPIM_PSEL_SCK      0x600
#define SPIM_PSEL_MOSI     0x604
#define SPIM_PSEL_MISO     0x608
#define SPIM_PSEL_CSN      0x610
#define SPIM_DMA_RX_PTR    0x704
#define SPIM_DMA_RX_MAXCNT 0x708
#define SPIM_DMA_TX_PTR    0x73c
#define SPIM_DMA_TX_MAXCNT 0x740

#define SPIM_MAXCNT        0xffff // DMA MAXCNT field is 16-bit


typedef struct nrf54l_spi {
  spi_t spi;
  device_t device;
  uint32_t base_addr;
  uint32_t base_clock;
  mutex_t mutex;
  task_waitable_t waitq;
  uint8_t done;
  uint8_t running;
  gpio_t clk;
  gpio_t miso;
  gpio_t mosi;
  uint8_t bounce_buffer[32];
} nrf54l_spi_t;


static void
spi_xfer_init(nrf54l_spi_t *spi, gpio_t nss, int config)
{
  if(!spi->running) {
    spi->running = 1;
    // gpio_t encodes (port<<5)|pin, which is exactly the PSEL field layout.
    reg_wr(spi->base_addr + SPIM_PSEL_SCK, spi->clk);
    reg_wr(spi->base_addr + SPIM_PSEL_MOSI, spi->mosi);
    reg_wr(spi->base_addr + SPIM_PSEL_MISO, spi->miso);
    reg_wr(spi->base_addr + SPIM_PSEL_CSN, 0xffffffff); // CSN unused (manual)
    reg_wr(spi->base_addr + SPIM_ENABLE, 7);
    reg_wr(spi->base_addr + SPIM_INTENSET, SPIM_INTENSET_END);
  }

  reg_wr(spi->base_addr + SPIM_CONFIG, config & 0x7);
  reg_wr(spi->base_addr + SPIM_PRESCALER, (config >> 8) & 0x7f);
  gpio_set_output(nss, 0); // assert CS
}


static void
spi_xfer_fini(nrf54l_spi_t *spi, gpio_t nss)
{
  gpio_set_output(nss, 1); // deassert CS
}


static void
spi_xfer(nrf54l_spi_t *spi, const uint8_t *tx, uint8_t *rx, size_t len)
{
  // EasyDMA can only reach RAM; TX from RRAM/code needs a bounce buffer.
  const int bounce = tx && (intptr_t)tx < 0x20000000;

  while(len) {
    size_t chunk;

    if(bounce) {
      chunk = MIN(len, sizeof(spi->bounce_buffer));
      memcpy(spi->bounce_buffer, tx, chunk);
      reg_wr(spi->base_addr + SPIM_DMA_TX_PTR, (intptr_t)spi->bounce_buffer);
    } else {
      chunk = MIN(len, SPIM_MAXCNT);
      reg_wr(spi->base_addr + SPIM_DMA_TX_PTR, (intptr_t)tx);
    }

    reg_wr(spi->base_addr + SPIM_DMA_RX_PTR, (intptr_t)rx);
    reg_wr(spi->base_addr + SPIM_DMA_RX_MAXCNT, rx ? chunk : 0);
    reg_wr(spi->base_addr + SPIM_DMA_TX_MAXCNT, tx ? chunk : 0);

    int q = irq_forbid(IRQ_LEVEL_SCHED);

    reg_wr(spi->base_addr + SPIM_EVENTS_END, 0);
    reg_wr(spi->base_addr + SPIM_TASKS_START, 1);

    while(!spi->done) {
      task_sleep_sched_locked(&spi->waitq);
    }
    spi->done = 0;

    irq_permit(q);

    if(rx)
      rx += chunk;
    if(tx)
      tx += chunk;
    len -= chunk;
  }
}


static error_t
spi_rw_locked(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len,
              gpio_t nss, int config)
{
  nrf54l_spi_t *spi = (nrf54l_spi_t *)dev;
  spi_xfer_init(spi, nss, config);
  spi_xfer(spi, tx, rx, len);
  spi_xfer_fini(spi, nss);
  return 0;
}


static error_t
spi_rw(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss,
       int config)
{
  nrf54l_spi_t *spi = (nrf54l_spi_t *)dev;
  mutex_lock(&spi->mutex);
  error_t err = spi_rw_locked(&spi->spi, tx, rx, len, nss, config);
  mutex_unlock(&spi->mutex);
  return err;
}


static error_t
spi_rwv(struct spi *dev, const struct iovec *txiov,
        const struct iovec *rxiov, size_t count, gpio_t nss, int config)
{
  nrf54l_spi_t *spi = (nrf54l_spi_t *)dev;
  mutex_lock(&spi->mutex);

  spi_xfer_init(spi, nss, config);
  for(size_t i = 0; i < count; i++) {
    spi_xfer(spi, txiov[i].iov_base,
             rxiov ? rxiov[i].iov_base : NULL, txiov[i].iov_len);
  }
  spi_xfer_fini(spi, nss);

  mutex_unlock(&spi->mutex);
  return 0;
}


static int
spi_get_config(spi_t *dev, int clock_flags, int baudrate)
{
  nrf54l_spi_t *spi = (nrf54l_spi_t *)dev;

  uint32_t divisor = spi->base_clock / baudrate;
  if(divisor < 2)
    divisor = 2;
  if(divisor > 126)
    divisor = 126;

  uint32_t r = (divisor & 0x7f) << 8;
  if(clock_flags & SPI_CPOL)
    r |= 0x4;
  if(clock_flags & SPI_CPHA)
    r |= 0x2;
  return r;
}


static void
spi_irq(void *arg)
{
  nrf54l_spi_t *spi = arg;
  if(reg_rd(spi->base_addr + SPIM_EVENTS_END)) {
    reg_wr(spi->base_addr + SPIM_EVENTS_END, 0);
    spi->done = 1;
    task_wakeup_sched_locked(&spi->waitq, 0);
  }
}


static void
spi_lock(spi_t *dev, int acquire)
{
  nrf54l_spi_t *spi = (nrf54l_spi_t *)dev;
  if(acquire)
    mutex_lock(&spi->mutex);
  else
    mutex_unlock(&spi->mutex);
}


static void
nrf54l_spi_power_state(struct device *dev, device_power_state_t state)
{
  nrf54l_spi_t *spi = (void *)dev - offsetof(nrf54l_spi_t, device);
  // rw() holds the mutex for the whole transfer, so taking it here means we
  // never disable the peripheral mid-transaction.
  mutex_lock(&spi->mutex);
  if(state == DEVICE_POWER_STATE_SUSPEND) {
    reg_wr(spi->base_addr + SPIM_ENABLE, 0);
    spi->running = 0; // re-init pins/enable on the next transfer
  }
  mutex_unlock(&spi->mutex);
}


static const device_class_t nrf54l_spi_device_class = {
  .dc_class_name = "spi",
  .dc_power_state = nrf54l_spi_power_state,
};


spi_t *
nrf54l_spi_create(uint32_t base_addr, int irq, uint32_t base_clock,
                  gpio_t clk, gpio_t miso, gpio_t mosi)
{
  nrf54l_spi_t *spi = calloc(1, sizeof(nrf54l_spi_t));

  spi->base_addr = base_addr;
  spi->base_clock = base_clock;
  spi->clk = clk;
  spi->miso = miso;
  spi->mosi = mosi;

  mutex_init(&spi->mutex, "spi");
  task_waitable_init(&spi->waitq, "spi");

  // SCK/MOSI are driven by the SPIM (output, input buffer kept connected,
  // which the SPIM requires on SCK); MISO is sampled as an input.
  gpio_conf_output(clk, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_output(mosi, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_input(miso, GPIO_PULL_NONE);

  spi->spi.rw = spi_rw;
  spi->spi.rwv = spi_rwv;
  spi->spi.rw_locked = spi_rw_locked;
  spi->spi.lock = spi_lock;
  spi->spi.get_config = spi_get_config;

  irq_enable_fn_arg(irq, IRQ_LEVEL_SCHED, spi_irq, spi);

  spi->device.d_name = "spi";
  spi->device.d_class = &nrf54l_spi_device_class;
  device_register(&spi->device);

  printf("SPI at 0x%x IRQ %d\n", (unsigned)base_addr, irq);
  return &spi->spi;
}
