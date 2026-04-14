#include "stm32n6_spi.h"
#include "stm32n6_clk.h"
#include "stm32n6_reg.h"
#include "stm32n6_dma.h"

#include <mios/type_macros.h>
#include <mios/task.h>
#include <sys/uio.h>
#include <sys/param.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "irq.h"
#include "cache.h"

typedef struct stm32n6_spi {
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

} stm32n6_spi_t;


static const struct {
  uint32_t reg_base;
  uint16_t clkid;
  uint8_t tx_dma;
  uint8_t rx_dma;
  uint8_t irq;
  uint8_t fifo_size;
} spi_config[] = {
  { SPI1_BASE, CLK_SPI1, STM32N6_DMA_SPI1_TX, STM32N6_DMA_SPI1_RX, 153, 16 },
  { SPI2_BASE, CLK_SPI2, STM32N6_DMA_SPI2_TX, STM32N6_DMA_SPI2_RX, 154, 16 },
  { SPI3_BASE, CLK_SPI3, STM32N6_DMA_SPI3_TX, STM32N6_DMA_SPI3_RX, 155, 16 },
  { SPI4_BASE, CLK_SPI4, STM32N6_DMA_SPI4_TX, STM32N6_DMA_SPI4_RX, 156, 8 },
  { SPI5_BASE, CLK_SPI5, STM32N6_DMA_SPI5_TX, STM32N6_DMA_SPI5_RX, 157, 8 },
  { SPI6_BASE, CLK_SPI6, STM32N6_DMA_SPI6_TX, STM32N6_DMA_SPI6_RX, 158, 8 },
};


static error_t
spi_xfer_pio(struct stm32n6_spi *spi, const uint8_t *tx, uint8_t *rx,
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
spi_xfer(struct stm32n6_spi *spi, const uint8_t *tx, uint8_t *rx,
         size_t len, uint32_t cfg1, uint32_t cfg2)
{
  // DMA requires buffers in AXISRAM1/2 (0x24000000 - 0x241FFFFF)
  // AXISRAM3-6 (>= 0x24200000) are on the NPU interconnect, not DMA-accessible
  #define ADDR_NOT_DMA(p) ((intptr_t)(p) < 0x24000000 || (intptr_t)(p) >= 0x24200000)

  if(spi->rx_dma == STM32_DMA_INSTANCE_NONE ||
     len < 4 ||
     (tx && ADDR_NOT_DMA(tx)) ||
     (rx && ((len & (CACHE_LINE_SIZE - 1)) ||
             ADDR_NOT_DMA(rx) ||
             ((intptr_t)rx & (CACHE_LINE_SIZE - 1))))) {
    return spi_xfer_pio(spi, tx, rx, len, cfg1, cfg2);
  }

  reg_wr(spi->reg_base + SPI_CFG2,
         cfg2                  |
         (rx ? 0 : (1 << 17))  |
         (tx ? 0 : (1 << 18))  |
         0);

  reg_wr(spi->reg_base + SPI_CR2, len);

  // Set DMA enable bits in CFG1 BEFORE SPE — N6 SPI requires
  // CFG1 to be written while SPI is disabled
  if(rx) {
    cfg1 |= 1 << 14; // RXDMAEN
    dcache_op(rx, len, DCACHE_INVALIDATE);
    stm32_dma_set_mem0(spi->rx_dma, rx);
    stm32_dma_set_nitems(spi->rx_dma, len);
    stm32_dma_start(spi->rx_dma);
  }

  if(tx) {
    cfg1 |= 1 << 15; // TXDMAEN
    dcache_op((void *)tx, len, DCACHE_CLEAN);
    stm32_dma_set_mem0(spi->tx_dma, (void *)tx);
    stm32_dma_set_nitems(spi->tx_dma, len);
    stm32_dma_start(spi->tx_dma);
  }

  reg_wr(spi->reg_base + SPI_CFG1, cfg1);  // Write RXDMAEN/TXDMAEN while SPE=0
  reg_wr(spi->reg_base + SPI_CR1, (1 << 0));  // Enable SPI (SPE=1)

  int q = irq_forbid(IRQ_LEVEL_SCHED);

  reg_wr(spi->reg_base + SPI_CR1,
         (1 << 9) | // CSTART
         (1 << 0) | // Enable
         0);

  reg_wr(spi->reg_base + SPI_IER, (1 << 3)); // End-of-transfer IE

  while(!spi->eot) {
    task_sleep_sched_locked(&spi->waitq);
  }
  spi->eot = 0;
  irq_permit(q);

  reg_wr(spi->reg_base + SPI_IER, 0);

  if(rx)
    stm32_dma_stop(spi->rx_dma);
  if(tx)
    stm32_dma_stop(spi->tx_dma);

  reg_wr(spi->reg_base + SPI_CR1, 0);
  reg_wr(spi->reg_base + SPI_CFG1, cfg1 & ~0xc000); // Clear RXDMAEN/TXDMAEN
  reg_wr(spi->reg_base + SPI_IFCR, 0xffff);
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


static void
tx_dma_error(stm32_dma_instance_t instance, uint32_t status, void *arg)
{
  stm32n6_spi_t *spi = arg;
  panic("%s: tx-dma error 0x%x", spi->name, status);
}

static void
rx_dma_error(stm32_dma_instance_t instance, uint32_t status, void *arg)
{
  stm32n6_spi_t *spi = arg;
  panic("%s: rx-dma error 0x%x", spi->name, status);
}


spi_t *
stm32n6_spi_create_unit(unsigned int instance, int flags)
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

  if(flags & STM32N6_SPI_NO_DMA) {
    spi->rx_dma = STM32_DMA_INSTANCE_NONE;
    spi->tx_dma = STM32_DMA_INSTANCE_NONE;
  } else {
    spi->rx_dma = stm32_dma_alloc(spi_config[instance].rx_dma, "spirx");
    stm32_dma_set_callback(spi->rx_dma, rx_dma_error, spi, IRQ_LEVEL_CLOCK,
                           DMA_STATUS_XFER_ERROR);

    spi->tx_dma = stm32_dma_alloc(spi_config[instance].tx_dma, "spitx");
    stm32_dma_set_callback(spi->tx_dma, tx_dma_error, spi, IRQ_LEVEL_CLOCK,
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
  }

  irq_enable_fn_arg(spi_config[instance].irq, IRQ_LEVEL_SCHED,
                    stm32n6_spi_irq, spi);

  spi->spi.rw = spi_rw;
  spi->spi.rwv = spi_rwv;
  spi->spi.rw_locked = spi_rw_locked;
  spi->spi.lock = spi_lock;
  spi->spi.get_config = spi_get_config;
  return &spi->spi;
}
