#include <stdio.h>
#include <assert.h>

#include <sys/uio.h>
#include <mios/io.h>
#include <mios/task.h>

#include "stm32_dma.h"

#include <stdlib.h>

#include "irq.h"

typedef struct stm32_spi {
  spi_t spi;
  uint32_t base_addr;
  mutex_t mutex;

  stm32_dma_instance_t rx_dma;
  stm32_dma_instance_t tx_dma;
  uint16_t clkid;
} stm32_spi_t;

#define SPI_CR1  0x00
#define SPI_CR2  0x04
#define SPI_SR   0x08
#define SPI_DR   0x0c

#define CR2_VALUE (1 << 12) // Should be set for 8 bit wide transfers

static error_t
spi_dma(struct stm32_spi *spi, const uint8_t *tx, uint8_t *rx, size_t len,
        uint32_t config)
{
  stm32_dma_set_mem0(spi->tx_dma, (void *)tx);
  stm32_dma_set_nitems(spi->tx_dma, len);

  if(rx != NULL) {
    stm32_dma_set_mem0(spi->rx_dma, rx);
    stm32_dma_set_nitems(spi->rx_dma, len);
    reg_wr(spi->base_addr + SPI_CR2, CR2_VALUE | 1);
  }

  int q = irq_forbid(IRQ_LEVEL_SCHED);

  stm32_dma_start(spi->tx_dma);
  if(rx != NULL) {
    stm32_dma_start(spi->rx_dma);
  }

  reg_wr(spi->base_addr + SPI_CR2,
         CR2_VALUE |
         (1 << 1) |
         (rx ? 1 : 0));

  error_t err;
  err = rx != NULL ? stm32_dma_wait(spi->rx_dma) : 0;
  if(!err) {
    err = stm32_dma_wait(spi->tx_dma);
  }

  stm32_dma_stop(spi->tx_dma);
  if(rx != NULL) {
    stm32_dma_stop(spi->rx_dma);
  }
  irq_permit(q);

  if(rx == NULL) {
    while(reg_rd(spi->base_addr + SPI_SR) != 2) {
      (void)reg_rd(spi->base_addr + SPI_DR);
    }
  }

  reg_wr(spi->base_addr + SPI_CR2, CR2_VALUE);

  return err;
}


static error_t
spi_rw_locked(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len,
              gpio_t nss, int config)
{
  struct stm32_spi *spi = (struct stm32_spi *)dev;
  if(len == 0)
    return ERR_OK;

  gpio_set_output(nss, 0);
  reg_wr(spi->base_addr + SPI_CR1, config);

  error_t err = spi_dma(spi, tx, rx, len, config);

  gpio_set_output(nss, 1);
  return err;
}


static error_t
spi_rw(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss,
       int config)
{
  struct stm32_spi *spi = (struct stm32_spi *)dev;

  mutex_lock(&spi->mutex);
  error_t err = spi_rw_locked(&spi->spi, tx, rx, len, nss, config);
  mutex_unlock(&spi->mutex);
  return err;
}


static error_t
spi_txv(struct spi *bus, const struct iovec *txiov, size_t count,
        gpio_t nss, int config)
{
  struct stm32_spi *spi = (struct stm32_spi *)bus;
  error_t err = 0;
  mutex_lock(&spi->mutex);

  gpio_set_output(nss, 0);
  reg_wr(spi->base_addr + SPI_CR1, config);

  for(size_t i = 0; i < count; i++) {
    err = spi_dma(spi, txiov[i].iov_base, NULL, txiov[i].iov_len, config);
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
  struct stm32_spi *spi = (struct stm32_spi *)dev;
  if(acquire)
    mutex_lock(&spi->mutex);
  else
    mutex_unlock(&spi->mutex);
}



static int
spi_get_config(spi_t *dev, int clock_flags, int baudrate)
{
  struct stm32_spi *spi = (struct stm32_spi *)dev;

  int config =
    (1 << 9) | // SSM
    (1 << 8) | // SSI
    (1 << 6) | // Enable
    (1 << 2);  // Master configuration

  if(clock_flags & SPI_CPHA)
    config |= (1 << 0);
  if(clock_flags & SPI_CPOL)
    config |= (1 << 1);

  int f = clk_get_freq(spi->clkid) / 2;

  if(baudrate == 0)
    baudrate = 1;

  int d;
  for(d = 0; d < 7; d++) {
    if(baudrate >= f)
      break;
    f >>= 1;
  }
  config |= (d << 3); // Divider
  return config;
}



static spi_t *
stm32_spi_create(int reg_base, int clkid,
                 uint32_t tx_dma_resource_id,
                 uint32_t rx_dma_resource_id)
{
  stm32_spi_t *spi = calloc(1, sizeof(stm32_spi_t));

  spi->clkid = clkid;
  clk_enable(clkid);

  spi->base_addr = reg_base;
  mutex_init(&spi->mutex, "spi");
  reg_wr(spi->base_addr + SPI_CR1, spi_get_config(&spi->spi, 0, 1));

  spi->rx_dma = stm32_dma_alloc(rx_dma_resource_id, "spirx");
  stm32_dma_make_waitable(spi->rx_dma, "spirx");
  spi->tx_dma = stm32_dma_alloc(tx_dma_resource_id, "spitx");
  stm32_dma_make_waitable(spi->tx_dma, "spitx");

  stm32_dma_config(spi->rx_dma,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_PRIO_LOW,
                   STM32_DMA_8BIT,
                   STM32_DMA_8BIT,
                   STM32_DMA_INCREMENT,
                   STM32_DMA_FIXED,
                   STM32_DMA_SINGLE,
                   STM32_DMA_P_TO_M);

  stm32_dma_config(spi->tx_dma,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_PRIO_LOW,
                   STM32_DMA_8BIT,
                   STM32_DMA_8BIT,
                   STM32_DMA_INCREMENT,
                   STM32_DMA_FIXED,
                   STM32_DMA_SINGLE,
                   STM32_DMA_M_TO_P);

  stm32_dma_set_paddr(spi->rx_dma, spi->base_addr + SPI_DR);
  stm32_dma_set_paddr(spi->tx_dma, spi->base_addr + SPI_DR);


  spi->spi.rw = spi_rw;
  spi->spi.txv = spi_txv;
  spi->spi.rw_locked = spi_rw_locked;
  spi->spi.lock = spi_lock;
  spi->spi.get_config = spi_get_config;
  return &spi->spi;
}
