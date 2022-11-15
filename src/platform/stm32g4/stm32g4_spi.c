#include "stm32g4_reg.h"
#include "stm32g4_clk.h"

#include "platform/stm32/stm32_spi.c"




static const struct {
  uint32_t reg_base;
  uint32_t tx_dma;
  uint32_t rx_dma;
  uint16_t clkid;
  uint8_t af;
} spi_config[] = {
  { 0x40013000, 11, 10, CLK_SPI1, 5 }
};



spi_t *
stm32g4_spi_create(unsigned int instance, gpio_t clk, gpio_t miso,
                   gpio_pull_t mosi)
{
  instance--;

  if(instance > ARRAYSIZE(spi_config))
    panic("spi: Invalid instance %d", instance + 1);

  const uint8_t af = spi_config[instance].af;
  gpio_conf_af(clk,  af, GPIO_PUSH_PULL,  GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(miso, af, GPIO_OPEN_DRAIN, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(mosi, af, GPIO_PUSH_PULL,  GPIO_SPEED_LOW, GPIO_PULL_NONE);

  return stm32_spi_create(spi_config[instance].reg_base,
                          spi_config[instance].clkid,
                          spi_config[instance].tx_dma,
                          spi_config[instance].rx_dma);
}
