#include "stm32n6_spi.h"
#include "stm32n6_clk.h"
#include "stm32n6_reg.h"

#include <mios/type_macros.h>
#include <mios/task.h>
#include <sys/uio.h>
#include <sys/param.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "irq.h"

typedef struct stm32n6_spi {
  spi_t spi;
  uint32_t reg_base;
  mutex_t mutex;

  task_waitable_t waitq;

  uint16_t clkid;
  uint8_t eot;
  uint8_t fifo_size;

  char name[6];

} stm32n6_spi_t;


static const struct {
  uint32_t reg_base;
  uint16_t clkid;
  uint8_t irq;
  uint8_t fifo_size;
} spi_config[] = {
  { SPI1_BASE, CLK_SPI1, 153, 16 },
  { SPI2_BASE, CLK_SPI2, 154, 16 },
  { SPI3_BASE, CLK_SPI3, 155, 16 },
  { SPI4_BASE, CLK_SPI4, 156, 8 },
  { SPI5_BASE, CLK_SPI5, 157, 8 },
  { SPI6_BASE, CLK_SPI6, 158, 8 },
};


static error_t
spi_xfer(struct stm32n6_spi *spi, const uint8_t *tx, uint8_t *rx,
         size_t len, uint32_t cfg1, uint32_t cfg2)
{
  reg_wr(spi->reg_base + SPI_CFG2,
         cfg2                 |
         (rx ? 0 : (1 << 17)) |  // Simplex TX (no RX) when rx is NULL
         (tx ? 0 : (1 << 18)) |  // Simplex RX (no TX) when tx is NULL
         0);

  while(len) {
    ssize_t xferlen = MIN(len, spi->fifo_size);

    reg_wr(spi->reg_base + SPI_CR2, xferlen);

    reg_wr(spi->reg_base + SPI_CR1, (1 << 0));  // Enable SPI

    // Load TX FIFO before starting transfer
    if(tx) {
      ssize_t i;
      for(i = 0; i < xferlen - 3; i += 4) {
        reg_wr(spi->reg_base + SPI_TXDR, *(uint32_t *)&tx[i]);
      }
      for(; i < xferlen; i++) {
        reg_wr8(spi->reg_base + SPI_TXDR, tx[i]);
      }
      tx += xferlen;
    }

    reg_wr(spi->reg_base + SPI_CR1,
           (1 << 9) | // CSTART
           (1 << 0) | // Enable
           0);

    int q = irq_forbid(IRQ_LEVEL_SCHED);
    reg_wr(spi->reg_base + SPI_IER, (1 << 3));
    while(!spi->eot) {
      task_sleep_sched_locked(&spi->waitq);
    }
    reg_wr(spi->reg_base + SPI_IER, 0);
    spi->eot = 0;
    irq_permit(q);

    if(rx) {
      ssize_t i;
      for(i = 0; i < xferlen - 3; i += 4) {
        *(uint32_t *)&rx[i] = reg_rd(spi->reg_base + SPI_RXDR);
      }
      for(; i < xferlen; i++) {
        rx[i] = reg_rd8(spi->reg_base + SPI_RXDR);
      }
      rx += xferlen;
    }

    reg_wr(spi->reg_base + SPI_CR1, 0);
    reg_wr(spi->reg_base + SPI_IFCR, 0xffff);
    len -= xferlen;
  }

  return 0;
}


static error_t
spi_rw_locked(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len,
              gpio_t nss, int config)
{
  struct stm32n6_spi *spi = (struct stm32n6_spi *)dev;
  if(len == 0)
    return ERR_OK;

  uint32_t cfg2 =
    ((config & 3) << 24)  | // CPHA & CPOL
    (1 << 28)             | // (internal) SS is active high
    (1 << 26)             | // Software management of SS
    (1 << 22)             | // Master mode
    (1 << 31)             | // Take control over IO even when disabled
    0;

  reg_wr(spi->reg_base + SPI_CFG1, cfg2);

  uint32_t cfg1 =
    (config & 0xf0000000) | // Baudrate
    (0b111 << 0)          | // DSIZE (8 bit)
    0;

  reg_wr(spi->reg_base + SPI_CFG1, cfg1);

  gpio_set_output(nss, 0);
  error_t err = spi_xfer(spi, tx, rx, len, cfg1, cfg2);
  gpio_set_output(nss, 1);
  return err;
}


static error_t
spi_rw(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss,
       int config)
{
  struct stm32n6_spi *spi = (struct stm32n6_spi *)dev;

  mutex_lock(&spi->mutex);
  error_t err = spi_rw_locked(&spi->spi, tx, rx, len, nss, config);
  mutex_unlock(&spi->mutex);
  return err;
}


static error_t
spi_rwv(struct spi *bus, const struct iovec *txiov,
        const struct iovec *rxiov, size_t count,
        gpio_t nss, int config)
{
  struct stm32n6_spi *spi = (struct stm32n6_spi *)bus;

  error_t err = 0;
  mutex_lock(&spi->mutex);

  uint32_t cfg2 =
    ((config & 3) << 24)  | // CPHA & CPOL
    (1 << 28)             | // (internal) SS is active high
    (1 << 26)             | // Software management of SS
    (1 << 22)             | // Master mode
    (1 << 31)             | // Take control over IO even when disabled
    0;

  reg_wr(spi->reg_base + SPI_CFG1, cfg2);

  uint32_t cfg1 =
    (config & 0xf0000000) | // Baudrate
    (0b111 << 0)          | // DSIZE (8 bit)
    0;

  reg_wr(spi->reg_base + SPI_CFG1, cfg1);

  gpio_set_output(nss, 0);

  for(size_t i = 0; i < count; i++) {
    err = spi_xfer(spi, txiov[i].iov_base,
                   rxiov ? rxiov[i].iov_base : NULL, txiov[i].iov_len,
                   cfg1, cfg2);
    if(err)
      break;
  }
  gpio_set_output(nss, 1);

  mutex_unlock(&spi->mutex);
  return err;
}


static void
spi_lock(spi_t *dev, int acquire)
{
  struct stm32n6_spi *spi = (struct stm32n6_spi *)dev;
  if(acquire)
    mutex_lock(&spi->mutex);
  else
    mutex_unlock(&spi->mutex);
}


static int
spi_get_config(spi_t *dev, int clock_flags, int baudrate)
{
  struct stm32n6_spi *spi = (struct stm32n6_spi *)dev;
  int f = clk_get_freq(spi->clkid) / 2;
  printf("f=%d\n", f);
  int mbr;
  for(mbr = 0; mbr < 7; mbr++) {
    if(baudrate >= f)
      break;
    f >>= 1;
  }
  return clock_flags | (mbr << 28);
}


static void
stm32n6_spi_irq(void *arg)
{
  struct stm32n6_spi *spi = arg;
  reg_wr(spi->reg_base + SPI_IFCR, 1 << 3);
  spi->eot = 1;
  task_wakeup_sched_locked(&spi->waitq, 0);
}


spi_t *
stm32n6_spi_create_unit(unsigned int instance)
{
  instance--;

  if(instance >= ARRAYSIZE(spi_config))
    panic("spi: Invalid instance %d", instance + 1);

  stm32n6_spi_t *spi = calloc(1, sizeof(stm32n6_spi_t));
  snprintf(spi->name, sizeof(spi->name), "spi%d", instance + 1);

  spi->clkid = spi_config[instance].clkid;
  clk_enable(spi->clkid);
  spi->reg_base = spi_config[instance].reg_base;
  spi->fifo_size = spi_config[instance].fifo_size;
  mutex_init(&spi->mutex, spi->name);
  task_waitable_init(&spi->waitq, spi->name);

  irq_enable_fn_arg(spi_config[instance].irq, IRQ_LEVEL_SCHED,
                    stm32n6_spi_irq, spi);

  spi->spi.rw = spi_rw;
  spi->spi.rwv = spi_rwv;
  spi->spi.rw_locked = spi_rw_locked;
  spi->spi.lock = spi_lock;
  spi->spi.get_config = spi_get_config;
  return &spi->spi;
}
