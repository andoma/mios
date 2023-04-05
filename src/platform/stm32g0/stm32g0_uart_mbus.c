#include "stm32g0_uart.h"

#include "stm32g0_clk.h"
#include "stm32g0_uart.h"

#include "platform/stm32/stm32_mbus_uart.c"

void
stm32g0_mbus_uart_create(unsigned int instance,
                         gpio_t tx, gpio_t rx, gpio_t txe,
                         int flags)
{
  const stm32g0_uart_cfg_t *cfg = stm32g0_uart_config_get(instance);
  const int tx_af = stm32g0_uart_tx(instance, tx);
  const int rx_af = stm32g0_uart_rx(instance, rx);

  gpio_conf_af(tx, tx_af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_UP);
  gpio_conf_af(rx, rx_af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_UP);
  if(txe != GPIO_UNUSED)
    gpio_conf_output(txe, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);


  unsigned int freq = clk_get_freq(cfg->clkid);

  if(flags & UART_WAKEUP) {
    // Run USART from HSI16 so it can resume us from STOP
    reg_set_bits(RCC_CCIPR, 2 * (instance - 1), 2, 2);
    freq = 16000000;
    if(txe != GPIO_UNUSED)
      gpio_conf_standby(txe, GPIO_PULL_DOWN);
  }

  const uint32_t baseaddr = (cfg->base << 8) + 0x40000000;

  stm32_mbus_uart_create(baseaddr,
                         cfg->clkid,
                         cfg->irq,
                         cfg->txdma,
                         cfg->rxdma,
                         txe, flags, freq);
}
