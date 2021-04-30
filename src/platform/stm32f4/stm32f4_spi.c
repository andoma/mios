#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <mios/io.h>
#include <mios/mios.h>
#include <mios/task.h>

#include "irq.h"
#include "stm32f4_reg.h"
#include "stm32f4_clk.h"
#include "stm32f4_spi.h"
#include "stm32f4_dma.h"

#define NAME "stm32f4_spi"

struct stm32f4_spi {
  spi_t spi;
  uint32_t base_addr;
  mutex_t mutex;

  stm32_dma_instance_t rx_dma;
  stm32_dma_instance_t tx_dma;
  uint8_t instance;
};

#define SPI_CR1  0x00
#define SPI_CR2  0x04
#define SPI_SR   0x08
#define SPI_DR   0x0c

static void
spi_dma(struct stm32f4_spi *spi, const uint8_t *tx, uint8_t *rx, size_t len)
{
  stm32_dma_set_mem0(spi->tx_dma, (void *)tx);
  stm32_dma_set_mem0(spi->rx_dma, rx);
  stm32_dma_set_nitems(spi->tx_dma, len);
  stm32_dma_set_nitems(spi->rx_dma, len);
  stm32_dma_start(spi->tx_dma);
  stm32_dma_start(spi->rx_dma);

  reg_wr(spi->base_addr + SPI_CR2, 3);

  error_t err;

  err = stm32_dma_wait(spi->rx_dma);
  assert(err == 0);
  err = stm32_dma_wait(spi->tx_dma);
  assert(err == 0);
  reg_wr(spi->base_addr + SPI_CR2, 0);
}


static error_t
spi_rw_locked(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len,
              gpio_t nss, int config)
{
  struct stm32f4_spi *spi = (struct stm32f4_spi *)dev;
  if(len == 0)
    return ERR_OK;

  gpio_set_output(nss, 0);
  reg_wr(spi->base_addr + SPI_CR1, config);

  int s = irq_forbid(IRQ_LEVEL_DMA);
  spi_dma(spi, tx, rx, len);
  irq_permit(s);

  gpio_set_output(nss, 1);
  return ERR_OK;
}


static error_t
spi_rw(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss,
       int config)
{
  struct stm32f4_spi *spi = (struct stm32f4_spi *)dev;

  mutex_lock(&spi->mutex);
  error_t err = spi_rw_locked(&spi->spi, tx, rx, len, nss, config);
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




static int
spi_get_config(spi_t *dev, int clock_flags, int baudrate)
{
  struct stm32f4_spi *spi = (struct stm32f4_spi *)dev;

  int config =
    (1 << 9) | // SSM
    (1 << 8) | // SSI
    (1 << 6) | // SPI Enable
    (1 << 2);  // Master configuration

  if(clock_flags & SPI_CPHA)
    config |= (1 << 0);
  if(clock_flags & SPI_CPOL)
    config |= (1 << 1);

  int f = clk_get_freq(spi_config[spi->instance].clkid) / 2;

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



spi_t *
stm32f4_spi_create(unsigned int instance, gpio_t clk, gpio_t miso,
                   gpio_pull_t mosi)
{
  instance--;

  if(instance > ARRAYSIZE(spi_config))
    panic("%s: Invalid instance %d", NAME, instance + 1);

  clk_enable(spi_config[instance].clkid);

  struct stm32f4_spi *spi = malloc(sizeof(struct stm32f4_spi));
  spi->instance = instance;
  spi->base_addr = (spi_config[instance].base << 8) + 0x40000000;
  mutex_init(&spi->mutex, "spi");
  reg_wr(spi->base_addr + SPI_CR1, spi_get_config(&spi->spi, 0, 1));

  switch(instance) {
  case 1:
    spi->rx_dma = stm32f4_dma_alloc_fixed(0, 3, 0, NULL, NULL, "spirx");
    spi->tx_dma = stm32f4_dma_alloc_fixed(0, 4, 0, NULL, NULL, "spitx");
    break;
  case 2:
    spi->rx_dma = stm32f4_dma_alloc_fixed(0, 0, 0, NULL, NULL, "spirx");
    spi->tx_dma = stm32f4_dma_alloc_fixed(0, 5, 0, NULL, NULL, "spitx");
    break;
  default:
    panic("Can't do SPI DMA for instance %d", instance + 1);
  }

  stm32_dma_config(spi->rx_dma,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_PRIO_LOW,
                   STM32_DMA_8BIT,
                   STM32_DMA_8BIT,
                   STM32_DMA_INCREMENT,
                   STM32_DMA_FIXED,
                   STM32_DMA_P_TO_M);

  stm32_dma_config(spi->tx_dma,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_BURST_NONE,
                   STM32_DMA_PRIO_LOW,
                   STM32_DMA_8BIT,
                   STM32_DMA_8BIT,
                   STM32_DMA_INCREMENT,
                   STM32_DMA_FIXED,
                   STM32_DMA_M_TO_P);

  stm32_dma_set_paddr(spi->rx_dma, spi->base_addr + SPI_DR);
  stm32_dma_set_paddr(spi->tx_dma, spi->base_addr + SPI_DR);

  // Configure GPIO
  const uint8_t af = spi_config[instance].af;
  gpio_conf_af(clk,  af, GPIO_PUSH_PULL,  GPIO_SPEED_HIGH, GPIO_PULL_UP);
  gpio_conf_af(miso, af, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(mosi, af, GPIO_PUSH_PULL,  GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  spi->spi.rw = spi_rw;
  spi->spi.rw_locked = spi_rw_locked;
  spi->spi.lock = spi_lock;
  spi->spi.get_config = spi_get_config;
  return &spi->spi;
}
