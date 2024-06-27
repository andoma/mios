#include "stm32f4_uart.h"

#include "stm32f4_clk.h"
#include "stm32f4_dma.h"

#include "platform/stm32/stm32_uart_mbus_multidrop.c"

void
stm32f4_uart_mbus_multidrop_create(unsigned int instance,
                                   gpio_t tx, gpio_t rx, gpio_t txe,
                                   const char *name)
{
  const int index = instance - 1;
  const stm32f4_uart_config_t *cfg = stm32f4_uart_get_config(index);
  const int af = cfg->af;

  gpio_conf_af(tx, af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_UP);
  gpio_conf_af(rx, af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_UP);
  gpio_conf_output(txe, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  stm32_mbus_uart_create((cfg->base << 8) + 0x40000000,
                         cfg->clkid,
                         cfg->irq,
                         cfg->txdma,
                         cfg->rxdma,
                         txe, 0,
                         clk_get_freq(cfg->clkid),
                         name);
}
