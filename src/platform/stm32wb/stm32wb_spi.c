#include "stm32wb_reg.h"
#include "stm32wb_clk.h"

#include "platform/stm32/stm32_spi.c"




static const struct {
  uint32_t reg_base;
  uint32_t tx_dma;
  uint32_t rx_dma;
  uint16_t clkid;
  uint8_t af;
} spi_config[] = {
  { 0x40013000, 7, 6, CLK_SPI1, 5 }
};



spi_t *
stm32wb_spi_create(unsigned int instance, gpio_t clk, gpio_t miso,
                   gpio_t mosi, gpio_output_speed_t speed)
{
  instance--;

  if(instance > ARRAYSIZE(spi_config))
    panic("spi: Invalid instance %d", instance + 1);

  const uint8_t af = spi_config[instance].af;
  gpio_conf_af(clk,  af, GPIO_PUSH_PULL,  speed, GPIO_PULL_NONE);
  gpio_conf_af(miso, af, GPIO_OPEN_DRAIN, speed, GPIO_PULL_UP);
  gpio_conf_af(mosi, af, GPIO_PUSH_PULL,  speed, GPIO_PULL_NONE);

  return stm32_spi_create(spi_config[instance].reg_base,
                          spi_config[instance].clkid,
                          spi_config[instance].tx_dma,
                          spi_config[instance].rx_dma);
}
