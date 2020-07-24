#include <assert.h>
#include <stdio.h>

#include "irq.h"
#include "stm32f4.h"
#include "gpio.h"
#include "mios.h"
#include "task.h"

#include "spi.h"

struct spi {
  uint32_t base_addr;

};

#define SPI_CR1  0x00
#define SPI_CR2  0x04
#define SPI_SR   0x08
#define SPI_DR   0x0c

struct spi spi2;


error_t
spi_rw(spi_t *spi, const uint8_t *tx, uint8_t *rx, size_t len)
{
  int x;
  for(size_t i = 0; i < len; i++) {
    x = 0;
    while((reg_rd(spi->base_addr + SPI_SR) & 2) == 0) {
      x++;
      if(x == 1000000)
        return ERR_TX;
    }
    reg_wr(spi->base_addr + SPI_DR, tx[i]);
    x = 0;
    while((reg_rd(spi->base_addr + SPI_SR) & 1) == 0) {
      x++;
      if(x == 1000000)
        return ERR_RX;
    }
    uint8_t b = reg_rd(spi->base_addr + SPI_DR);
    if(rx != NULL)
      rx[i] = b;
  }
  return ERR_OK;
}

static void __attribute__((constructor(200)))
init_spi(void)
{
  reg_set(RCC_APB1ENR, 1 << 14);  // CLK ENABLE: SPI2

  spi_t *spi = &spi2;
  spi->base_addr = 0x40003800;

  reg_wr(spi->base_addr + SPI_CR1,
         (1 << 9) |
         (1 << 8) |
         (1 << 2) |
         (7 << 3));

  reg_set(spi->base_addr + SPI_CR1,
          (1 << 6));

  // Configure PB13...PB15 SPI
  gpio_conf_af(GPIO_PB(13), 5, // CLK
               GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);
  gpio_conf_af(GPIO_PB(14), 5, // MISO
               GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(GPIO_PB(15), 5, // MOSI
               GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
}

