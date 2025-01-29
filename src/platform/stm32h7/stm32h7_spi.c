#include "stm32h7_spi.h"

#include "stm32h7_clk.h"
#include "stm32h7_dma.h"

#include <mios/mios.h>
#include <mios/task.h>
#include <sys/uio.h>

#include <stdlib.h>

typedef struct stm32h7_spi {
  spi_t spi;
  uint32_t reg_base;
  mutex_t mutex;

  stm32_dma_instance_t rx_dma;
  stm32_dma_instance_t tx_dma;
  uint16_t clkid;

} stm32h7_spi_t;


static const struct {
  uint32_t reg_base;
  uint8_t tx_dma;
  uint8_t rx_dma;
  uint16_t clkid;
  uint8_t af;
} spi_config[] = {
  { SPI1_BASE, 38, 37, CLK_SPI1, 5 },
  { SPI2_BASE, 40, 39, CLK_SPI2, 5 },
  { SPI3_BASE, 62, 61, CLK_SPI3, 6 },
};


static error_t
spi_xfer(struct stm32h7_spi *spi, const uint8_t *tx, uint8_t *rx, size_t len,
         uint32_t config)
{
  reg_wr(spi->reg_base + SPI_CFG1,
         (0b011 << 28) | // Baudrate wat?
         (0b111 << 0)  | // DSIZE (8 bit)
         0);

  reg_wr(spi->reg_base + SPI_CFG2,
         ((config & 3) << 24) | // CPHA & CPOL
         (1 << 28)            | // (internal) SS is active high
         (1 << 26)            | // Software management of SS
         (1 << 22)            | // Master mode
         (1 << 31)            | // Take control over IO even when disabled
         0);

  reg_wr(spi->reg_base + SPI_CR2, len);

  reg_wr(spi->reg_base + SPI_CR1,
         (1 << 0) | // Enable
         0);

  reg_wr(spi->reg_base + SPI_CR1,
         (1 << 9) | // master transfer enable
         (1 << 0) | // Enable
         0);

  for(size_t i = 0; i < len; i++) {
    while(reg_get_bit(spi->reg_base + SPI_SR, 1) == 0) {}

    reg_wr8(spi->reg_base + SPI_TXDR, tx ? tx[i] : 0);
    while(reg_get_bit(spi->reg_base + SPI_SR, 0) == 0) {
    }
    uint8_t r = reg_rd8(spi->reg_base + SPI_RXDR);
    if(rx)
      rx[i] = r;
  }
  reg_wr(spi->reg_base + SPI_CR1, 0);

  reg_wr(spi->reg_base + SPI_IFCR, 0xffff);
  return 0;
}


static error_t
spi_rw_locked(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len,
              gpio_t nss, int config)
{
  struct stm32h7_spi *spi = (struct stm32h7_spi *)dev;
  if(len == 0)
    return ERR_OK;

  gpio_set_output(nss, 0);

  error_t err = spi_xfer(spi, tx, rx, len, config);
  gpio_set_output(nss, 1);
  return err;
}


static error_t
spi_rw(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss,
       int config)
{
  struct stm32h7_spi *spi = (struct stm32h7_spi *)dev;

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
  struct stm32h7_spi *spi = (struct stm32h7_spi *)bus;

  error_t err = 0;
  mutex_lock(&spi->mutex);

  gpio_set_output(nss, 0);

  for(size_t i = 0; i < count; i++) {
    err = spi_xfer(spi, txiov[i].iov_base,
                  rxiov ? rxiov[i].iov_base : NULL, txiov[i].iov_len, config);
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
  struct stm32h7_spi *spi = (struct stm32h7_spi *)dev;
  if(acquire)
    mutex_lock(&spi->mutex);
  else
    mutex_unlock(&spi->mutex);
}



static int
spi_get_config(spi_t *dev, int clock_flags, int baudrate)
{
  return clock_flags;
}


spi_t *
stm32h7_spi_create(unsigned int instance, gpio_t clk, gpio_t miso,
                   gpio_t mosi, gpio_output_speed_t speed)
{
  instance--;

  if(instance > ARRAYSIZE(spi_config))
    panic("spi: Invalid instance %d", instance + 1);

  const uint8_t af = spi_config[instance].af;
  gpio_conf_af(clk,  af, GPIO_PUSH_PULL,  speed, GPIO_PULL_NONE);
  gpio_conf_af(miso, af, GPIO_OPEN_DRAIN, speed, GPIO_PULL_UP);
  gpio_conf_af(mosi, af, GPIO_PUSH_PULL,  speed, GPIO_PULL_NONE);

  stm32h7_spi_t *spi = calloc(1, sizeof(stm32h7_spi_t));

  spi->clkid = spi_config[instance].clkid;
  clk_enable(spi->clkid);
  spi->reg_base = spi_config[instance].reg_base;
  mutex_init(&spi->mutex, "spi");

  spi->rx_dma = stm32_dma_alloc(spi_config[instance].rx_dma, "spirx");
  stm32_dma_make_waitable(spi->rx_dma, "spirx");
  spi->tx_dma = stm32_dma_alloc(spi_config[instance].tx_dma, "spitx");
  stm32_dma_make_waitable(spi->tx_dma, "spitx");

  spi->spi.rw = spi_rw;
  spi->spi.rwv = spi_rwv;
  spi->spi.rw_locked = spi_rw_locked;
  spi->spi.lock = spi_lock;
  spi->spi.get_config = spi_get_config;
  return &spi->spi;
}
