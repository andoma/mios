#include <assert.h>
#include <stdio.h>
#include <io.h>

#include "irq.h"
#include "stm32f4.h"
#include "mios.h"
#include "task.h"

struct spi {
  uint32_t base_addr;

  mutex_t mutex;
  struct task_queue waitable;

  const uint8_t *tx;
  uint8_t *rx;
  size_t len;
  size_t pos;
};

#define SPI_CR1  0x00
#define SPI_CR2  0x04
#define SPI_SR   0x08
#define SPI_DR   0x0c

struct spi spi2;



error_t
spi_rw(spi_t *spi, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss)
{
  if(len == 0)
    return ERR_OK;
  mutex_lock(&spi->mutex);
  gpio_set_output(nss, 0);
  spi->tx = tx;
  spi->rx = rx;
  spi->len = len;
  spi->pos = 0;
  int s = irq_forbid(IRQ_LEVEL_IO);
  reg_wr(spi->base_addr + SPI_DR, tx[0]);
  task_sleep(&spi->waitable, 0);
  irq_permit(s);
  gpio_set_output(nss, 1);
  mutex_unlock(&spi->mutex);
  return ERR_OK;
}


static void
spi_irq(spi_t *spi)
{
  uint8_t b = reg_rd(spi->base_addr + SPI_DR);
  if(spi->rx)
    spi->rx[spi->pos] = b;
  spi->pos++;
  if(spi->pos == spi->len) {
    task_wakeup(&spi->waitable, 0);
    return;
  }
  reg_wr(spi->base_addr + SPI_DR, spi->tx[spi->pos]);
}


void
irq_36(void)
{
  spi_irq(&spi2);
}


static void __attribute__((constructor(200)))
init_spi(void)
{
  reg_set(RCC_APB1ENR, 1 << 14);  // CLK ENABLE: SPI2

  spi_t *spi = &spi2;
  spi->base_addr = 0x40003800;
  mutex_init(&spi->mutex);
  TAILQ_INIT(&spi->waitable);

  reg_wr(spi->base_addr + SPI_CR1,
         (1 << 9) |
         (1 << 8) |
         (1 << 6) |
         (1 << 2) |
         (7 << 3));

  reg_wr(spi->base_addr + SPI_CR2,
         (1 << 6));

  irq_enable(36, IRQ_LEVEL_IO);

  // Configure PB13...PB15 SPI
  gpio_conf_af(GPIO_PB(13), 5, // CLK
               GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);
  gpio_conf_af(GPIO_PB(14), 5, // MISO
               GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PB(15), 5, // MOSI
               GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
}

