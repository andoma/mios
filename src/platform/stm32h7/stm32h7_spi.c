#include "stm32h7_spi.h"

#include "stm32h7_clk.h"
#include "stm32h7_dma.h"

#include <mios/mios.h>
#include <mios/task.h>
#include <sys/uio.h>
#include <sys/param.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "irq.h"
#include "cache.h"

typedef struct stm32h7_spi {
  spi_t spi;
  uint32_t reg_base;
  mutex_t mutex;

  task_waitable_t waitq;

  stm32_dma_instance_t rx_dma;
  stm32_dma_instance_t tx_dma;
  uint16_t clkid;
  uint8_t eot;
  uint8_t fifo_size;

  char name[6];

} stm32h7_spi_t;


static const struct {
  uint32_t reg_base;
  uint8_t tx_dma;
  uint8_t rx_dma;
  uint16_t clkid;
  uint8_t af;
  uint8_t irq;
  uint8_t fifo_size;
} spi_config[] = {
  { SPI1_BASE, 38, 37, CLK_SPI1, 5, 35, 16 },
  { SPI2_BASE, 40, 39, CLK_SPI2, 5, 36, 16 },
  { SPI3_BASE, 62, 61, CLK_SPI3, 6, 51, 16 },
};



static error_t
spi_xfer_pio(struct stm32h7_spi *spi, const uint8_t *tx, uint8_t *rx,
             size_t len, uint32_t cfg1, uint32_t cfg2)
{
  reg_wr(spi->reg_base + SPI_CFG2,
         cfg2                 |
         (rx ? 0 : (1 << 17)) |
         (tx ? 0 : (1 << 18)) |
         0);

  while(len) {
    ssize_t xferlen = MIN(len, spi->fifo_size);

    reg_wr(spi->reg_base + SPI_CR2, xferlen);

    reg_wr(spi->reg_base + SPI_CR1,
           (1 << 0) | // Enable
           0);

    reg_wr(spi->reg_base + SPI_CR1,
           (1 << 9) | // master transfer enable
           (1 << 0) | // Enable
           0);

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
spi_xfer(struct stm32h7_spi *spi, const uint8_t *tx, uint8_t *rx, size_t len,
         uint32_t cfg1, uint32_t cfg2)
{
  if(len < 4 ||
     (rx && ((len & (CACHE_LINE_SIZE - 1)) ||
             ((intptr_t)rx & (CACHE_LINE_SIZE - 1))))) {
    return spi_xfer_pio(spi, tx, rx, len, cfg1, cfg2);
  }

  reg_wr(spi->reg_base + SPI_CFG2,
         cfg2                  |
         (rx ? 0 : (1 << 17))  |
         (tx ? 0 : (1 << 18))  |
         0);

  reg_wr(spi->reg_base + SPI_CR2, len);

  reg_wr(spi->reg_base + SPI_CR1,
         (1 << 0) | // Enable
         0);

  if(rx) {
    cfg1 |= 1 << 14; // RXDMAEN
    reg_wr(spi->reg_base + SPI_CFG1, cfg1);

    dcache_op(rx, len, DCACHE_INVALIDATE);
    stm32_dma_set_mem0(spi->rx_dma, rx);
    stm32_dma_set_nitems(spi->rx_dma, len);
    stm32_dma_start(spi->rx_dma);
  }

  if(tx) {
    dcache_op((void *)tx, len, DCACHE_CLEAN);
    stm32_dma_set_mem0(spi->tx_dma, (void *)tx);
    stm32_dma_set_nitems(spi->tx_dma, len);
    stm32_dma_start(spi->tx_dma);

    cfg1 |= 1 << 15; // TXDMAEN
    reg_wr(spi->reg_base + SPI_CFG1, cfg1);
  }

  int q = irq_forbid(IRQ_LEVEL_SCHED);

  reg_wr(spi->reg_base + SPI_CR1,
         (1 << 9) | // master transfer enable
         (1 << 0) | // Enable
         0);

  reg_wr(spi->reg_base + SPI_IER, (1 << 3)); // End-of-transfer enable

  while(!spi->eot) {
    task_sleep_sched_locked(&spi->waitq);
  }
  spi->eot = 0;
  irq_permit(q);

  reg_wr(spi->reg_base + SPI_IER, 0);

  if(rx) {
    stm32_dma_stop(spi->rx_dma);
  }
  if(tx) {
    stm32_dma_stop(spi->tx_dma);
  }

  reg_wr(spi->reg_base + SPI_CR1, 0);
  reg_wr(spi->reg_base + SPI_CFG1, cfg1 & ~0xc000); // Turn off DMA flags
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
  struct stm32h7_spi *spi = (struct stm32h7_spi *)dev;
  if(acquire)
    mutex_lock(&spi->mutex);
  else
    mutex_unlock(&spi->mutex);
}


static int
spi_get_config(spi_t *dev, int clock_flags, int baudrate)
{
  struct stm32h7_spi *spi = (struct stm32h7_spi *)dev;
  int f = clk_get_freq(spi->clkid) / 2;
  int mbr;
  for(mbr = 0; mbr < 7; mbr++) {
    if(baudrate >= f) {
      break;
    }
    f >>= 1;
  }
  return clock_flags | (mbr << 28);
}


static void
stm32h7_irq(void *arg)
{
  struct stm32h7_spi *spi = arg;
  reg_wr(spi->reg_base + SPI_IFCR, 1 << 3); // Clear end-of-transfer
  spi->eot = 1;
  task_wakeup_sched_locked(&spi->waitq, 0);
}

static void
tx_complete(stm32_dma_instance_t instance, uint32_t status, void *arg)
{
  stm32h7_spi_t *spi = arg;
  panic("%s: tx-dma error 0x%x", spi->name, status);
}

static void
rx_complete(stm32_dma_instance_t instance, uint32_t status, void *arg)
{
  stm32h7_spi_t *spi = arg;
  panic("%s: rx-dma error 0x%x", spi->name, status);
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
  snprintf(spi->name, sizeof(spi->name), "spi%d", instance + 1);

  spi->clkid = spi_config[instance].clkid;
  clk_enable(spi->clkid);
  spi->reg_base = spi_config[instance].reg_base;
  spi->fifo_size = spi_config[instance].fifo_size;
  mutex_init(&spi->mutex, spi->name);
  task_waitable_init(&spi->waitq, spi->name);

  spi->rx_dma = stm32_dma_alloc(spi_config[instance].rx_dma, "spirx");
  stm32_dma_set_callback(spi->rx_dma, rx_complete, spi, IRQ_LEVEL_CLOCK,
                         DMA_STATUS_XFER_ERROR);

  spi->tx_dma = stm32_dma_alloc(spi_config[instance].tx_dma, "spitx");
  stm32_dma_set_callback(spi->tx_dma, tx_complete, spi, IRQ_LEVEL_CLOCK,
                         DMA_STATUS_XFER_ERROR);

  stm32_dma_config_set_reg(spi->tx_dma,
                           stm32_dma_config_make_reg(STM32_DMA_BURST_NONE,
                                                     STM32_DMA_BURST_NONE,
                                                     STM32_DMA_PRIO_LOW,
                                                     STM32_DMA_8BIT,
                                                     STM32_DMA_8BIT,
                                                     STM32_DMA_INCREMENT,
                                                     STM32_DMA_FIXED,
                                                     STM32_DMA_SINGLE,
                                                     STM32_DMA_M_TO_P));

  stm32_dma_config_set_reg(spi->rx_dma,
                           stm32_dma_config_make_reg(STM32_DMA_BURST_NONE,
                                                     STM32_DMA_BURST_NONE,
                                                     STM32_DMA_PRIO_LOW,
                                                     STM32_DMA_8BIT,
                                                     STM32_DMA_8BIT,
                                                     STM32_DMA_INCREMENT,
                                                     STM32_DMA_FIXED,
                                                     STM32_DMA_SINGLE,
                                                     STM32_DMA_P_TO_M));

  stm32_dma_set_paddr(spi->rx_dma, spi->reg_base + SPI_RXDR);

  stm32_dma_set_paddr(spi->tx_dma, spi->reg_base + SPI_TXDR);

  irq_enable_fn_arg(spi_config[instance].irq, IRQ_LEVEL_SCHED, stm32h7_irq, spi);

  spi->spi.rw = spi_rw;
  spi->spi.rwv = spi_rwv;
  spi->spi.rw_locked = spi_rw_locked;
  spi->spi.lock = spi_lock;
  spi->spi.get_config = spi_get_config;
  return &spi->spi;
}
