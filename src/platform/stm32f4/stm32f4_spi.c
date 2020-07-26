#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <io.h>

#include "irq.h"
#include "stm32f4.h"
#include "stm32f4_clk.h"
#include "stm32f4_spi.h"
#include "mios.h"
#include "task.h"

#define NAME "stm32f4_spi"

struct stm32f4_spi {
  spi_t spi;

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

static error_t
spi_rw(spi_t *dev, const uint8_t *tx, uint8_t *rx, size_t len, gpio_t nss)
{
  struct stm32f4_spi *spi = (struct stm32f4_spi *)dev;
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
spi_irq(struct stm32f4_spi *spi)
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

static struct stm32f4_spi *spis[3];

void
irq_35(void)
{
  spi_irq(spis[0]);
}

void
irq_36(void)
{
  spi_irq(spis[1]);
}

void
irq_51(void)
{
  spi_irq(spis[2]);
}

static const struct {
  uint16_t base;
  uint16_t clkid;
  uint8_t irq;


} spi_config[] = {
  { 0x0130, CLK_SPI1, 35 },
  { 0x0038, CLK_SPI2, 36 },
  { 0x003c, CLK_SPI3, 51 },
};



// PB13, PB14, PB15

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

  spis[instance] = spi;
  irq_enable(spi_config[instance].irq, IRQ_LEVEL_IO);

  // Configure PB13...PB15 SPI
  uint8_t af = 5;
  gpio_conf_af(clk,  af, GPIO_PUSH_PULL,  GPIO_SPEED_HIGH, GPIO_PULL_UP);
  gpio_conf_af(miso, af, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(mosi, af, GPIO_PUSH_PULL,  GPIO_SPEED_HIGH, GPIO_PULL_NONE);

  spi->spi.rw = spi_rw;
  return &spi->spi;
}
