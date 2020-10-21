#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <io.h>

#include "irq.h"
#include "stm32f4.h"
#include "stm32f4_clk.h"
#include "stm32f4_spi.h"
#include "stm32f4_dma.h"
#include "mios.h"
#include "task.h"

#define NAME "stm32f4_spi"

struct stm32f4_spi {
  spi_t spi;
  uint32_t base_addr;
  mutex_t mutex;

  stm32f4_dma_instance_t rx_dma;
  stm32f4_dma_instance_t tx_dma;
};

#define SPI_CR1  0x00
#define SPI_CR2  0x04
#define SPI_SR   0x08
#define SPI_DR   0x0c

static void
spi_dma(struct stm32f4_spi *spi, const uint8_t *tx, uint8_t *rx, size_t len)
{
  stm32f4_dma_start(spi->tx_dma, (void *)tx, spi->base_addr + SPI_DR,
                    len, DMA_M_TO_P, 0);

  stm32f4_dma_start(spi->rx_dma, rx, spi->base_addr + SPI_DR,
                    len, DMA_P_TO_M, 0);

  reg_wr(spi->base_addr + SPI_CR2, 3);

  error_t err;

  err = stm32f4_dma_wait(spi->rx_dma);
  assert(err == 0);
  err = stm32f4_dma_wait(spi->tx_dma);
  assert(err == 0);
  reg_wr(spi->base_addr + SPI_CR2, 0);

}


static error_t
spi_rw_locked(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len,
              gpio_t nss)
{
  struct stm32f4_spi *spi = (struct stm32f4_spi *)dev;
  if(len == 0)
    return ERR_OK;

  gpio_set_output(nss, 0);

  int s = irq_forbid(IRQ_LEVEL_DMA);
  spi_dma(spi, tx, rx, len);
  irq_permit(s);

  gpio_set_output(nss, 1);
  return ERR_OK;
}


static error_t
spi_rw(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss)
{
  struct stm32f4_spi *spi = (struct stm32f4_spi *)dev;

  mutex_lock(&spi->mutex);
  error_t err = spi_rw_locked(&spi->spi, tx, rx, len, nss);
  mutex_unlock(&spi->mutex);
  return err;
}


static void
spi_lock(spi_t *dev, int acquire)
{
  struct stm32f4_spi *spi = (struct stm32f4_spi *)dev;
  if(acquire)
    mutex_lock(&spi->mutex);
  else
    mutex_unlock(&spi->mutex);
}


static const struct {
  uint16_t base;
  uint16_t clkid;
  uint8_t af;
} spi_config[] = {
  { 0x0130, CLK_SPI1, 5 },
  { 0x0038, CLK_SPI2, 5 },
  { 0x003c, CLK_SPI3, 6 },
};


spi_t *
stm32f4_spi_create(int instance, gpio_t clk, gpio_t miso,
                   gpio_pull_t mosi)
{
  if(instance < 1 || instance > 3)
    panic("%s: Invalid instance %d", NAME, instance);

  instance--;

  clk_enable(spi_config[instance].clkid);

  struct stm32f4_spi *spi = malloc(sizeof(struct stm32f4_spi));
  spi->base_addr = (spi_config[instance].base << 8) + 0x40000000;
  mutex_init(&spi->mutex, "spi");

  reg_wr(spi->base_addr + SPI_CR1,
         (1 << 9) |
         (1 << 8) |
         (1 << 6) |
         (1 << 2) |
         (2 << 3));

  assert(instance == 1); // Only DMA over SPI2 right now

  spi->rx_dma = stm32f4_dma_alloc_fixed(0, 3);
  spi->tx_dma = stm32f4_dma_alloc_fixed(0, 4);

  // Configure GPIO
  const uint8_t af = spi_config[instance].af;
  gpio_conf_af(clk,  af, GPIO_PUSH_PULL,  GPIO_SPEED_HIGH, GPIO_PULL_UP);
  gpio_conf_af(miso, af, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(mosi, af, GPIO_PUSH_PULL,  GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  spi->spi.rw = spi_rw;
  spi->spi.rw_locked = spi_rw_locked;
  spi->spi.lock = spi_lock;
  return &spi->spi;
}
