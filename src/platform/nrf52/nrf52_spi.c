#include <stdio.h>
#include <assert.h>
#include <string.h>

#include <sys/param.h>
#include <sys/uio.h>
#include <mios/io.h>
#include <mios/task.h>

#include <stdlib.h>

#include "irq.h"

#include "nrf52_reg.h"

#define SPIM_TASKS_START 0x010

#define SPIM_INTENSET   0x304
#define SPIM_INTENCLR   0x308

#define SPIM_EVENTS_END 0x118

#define SPIM_ENABLE     0x500
#define SPIM_PSEL_SCK   0x508
#define SPIM_PSEL_MOSI  0x50c
#define SPIM_PSEL_MISO  0x510

#define SPIM_FREQUENCY  0x524
#define SPIM_RXD_PTR    0x534
#define SPIM_RXD_MAXCNT 0x538
#define SPIM_RXD_AMOUNT 0x53c
#define SPIM_RXD_LIST   0x540
#define SPIM_TXD_PTR    0x544
#define SPIM_TXD_MAXCNT 0x548
#define SPIM_TXD_AMOUNT 0x54c
#define SPIM_TXD_LIST   0x550
#define SPIM_CONFIG     0x554


typedef struct nrf52_spi {
  spi_t spi;
  uint32_t base_addr;
  mutex_t mutex;
  task_waitable_t waitq;
  int done;
  uint8_t bounce_buffer[32];
} nrf52_spi_t;


static void
spi_xfer_init(nrf52_spi_t *spi, gpio_t nss, int config)
{
  reg_wr(spi->base_addr + SPIM_CONFIG, config & 0x7);
  reg_wr(spi->base_addr + SPIM_FREQUENCY, config & 0xff000000);
  gpio_set_output(nss, 0);
}

static void
spi_xfer_fini(nrf52_spi_t *spi, gpio_t nss)
{
  gpio_set_output(nss, 1);
}


static void
spi_xfer(nrf52_spi_t *spi, const uint8_t *tx, uint8_t *rx, size_t len)
{
  // Transfer from FLASH is not possible with EASYDMA
  // We need to use a bounce buffer
  const int bounce = tx && (intptr_t)tx < 0x20000000;

  while(len) {

    size_t chunk;

    if(bounce) {
      chunk = MIN(len, sizeof(spi->bounce_buffer));
      // Transfer from FLASH is not possible with EASYDMA
      // Copy to bounce buffer
      memcpy(spi->bounce_buffer, tx, chunk);
      reg_wr(spi->base_addr + SPIM_TXD_PTR, (intptr_t)spi->bounce_buffer);
    } else {
      chunk = MIN(len, 0xff);

      reg_wr(spi->base_addr + SPIM_TXD_PTR, (intptr_t)tx);
    }

    reg_wr(spi->base_addr + SPIM_RXD_PTR, (intptr_t)rx);

    reg_wr(spi->base_addr + SPIM_RXD_MAXCNT, rx ? chunk : 0);
    reg_wr(spi->base_addr + SPIM_TXD_MAXCNT, tx ? chunk : 0);

    int q = irq_forbid(IRQ_LEVEL_SCHED);

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
  nrf52_spi_t *spi = (nrf52_spi_t *)dev;
  spi_xfer_init(spi, nss, config);
  spi_xfer(spi, tx, rx, len);
  spi_xfer_fini(spi, nss);
  return 0;
}


static error_t
spi_rw(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss,
       int config)
{
  nrf52_spi_t *spi = (nrf52_spi_t *)dev;

  mutex_lock(&spi->mutex);
  error_t err = spi_rw_locked(&spi->spi, tx, rx, len, nss, config);
  mutex_unlock(&spi->mutex);
  return err;
}

static error_t
spi_rwv(struct spi *dev, const struct iovec *txiov,
        const struct iovec *rxiov, size_t count,
        gpio_t nss, int config)
{
  nrf52_spi_t *spi = (nrf52_spi_t *)dev;
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
  int freq = baudrate / 125000;
  if(freq < 1)
    freq = 1;
  if(freq > 64)
    freq = 64;

  uint32_t r = freq << 25;

  if(clock_flags & SPI_CPOL)
    r |= 0x4;
  if(clock_flags & SPI_CPHA)
    r |= 0x2;

  return r;
}


static void
spi_irq(void *arg)
{
  struct nrf52_spi *spi = (struct nrf52_spi *)arg;
  if(reg_rd(spi->base_addr + SPIM_EVENTS_END)) {
    reg_wr(spi->base_addr + SPIM_EVENTS_END, 0);
    spi->done = 1;
    task_wakeup_sched_locked(&spi->waitq, 0);
  }
}



static void
spi_lock(spi_t *dev, int acquire)
{
  struct nrf52_spi *spi = (struct nrf52_spi *)dev;
  if(acquire)
    mutex_lock(&spi->mutex);
  else
    mutex_unlock(&spi->mutex);
}


static const uint8_t spi_instance_to_sysid[3] = {3, 4, 35};

spi_t *
nrf52_spi_create(unsigned int spi_instance, gpio_t clk, gpio_t miso,
                 gpio_t mosi)
{
  if(spi_instance > 2)
    panic("spi: Invalid instance %d", spi_instance);

  nrf52_spi_t *spi = calloc(1, sizeof(nrf52_spi_t));

  const int sysid = spi_instance_to_sysid[spi_instance];
  spi->base_addr = 0x40000000 + (sysid << 12);

  mutex_init(&spi->mutex, "spi");
  task_waitable_init(&spi->waitq, "spi");

  reg_wr(spi->base_addr + SPIM_PSEL_SCK, clk);
  reg_wr(spi->base_addr + SPIM_PSEL_MISO, miso);
  reg_wr(spi->base_addr + SPIM_PSEL_MOSI, mosi);

  reg_wr(spi->base_addr + SPIM_ENABLE, 7); // SPIM
  reg_wr(spi->base_addr + SPIM_INTENSET, (1 << 6)); // END interrupt

  spi->spi.rw = spi_rw;
  spi->spi.rwv = spi_rwv;
  spi->spi.rw_locked = spi_rw_locked;
  spi->spi.lock = spi_lock;
  spi->spi.get_config = spi_get_config;
  irq_enable_fn_arg(sysid, IRQ_LEVEL_SCHED, spi_irq, spi);
  printf("SPI%d at 0x%x IRQ %d\n", spi_instance, spi->base_addr, sysid);
  return &spi->spi;
}

