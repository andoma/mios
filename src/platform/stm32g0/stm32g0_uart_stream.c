#include "stm32g0_uart.h"

#include "stm32g0_clk.h"
#include "stm32g0_uart.h"

#include "platform/stm32/stm32_uart.c"

static const stm32g0_uart_cfg_t uart_config[] = {
  { 0x0138, CLK_USART1, 27, 51, 50},
  { 0x0044, CLK_USART2, 28, 53, 52},
};


const stm32g0_uart_cfg_t *
stm32g0_uart_config_get(int instance)
{
  return &uart_config[instance - 1];
}


int stm32g0_uart_tx(int instance, gpio_t pin)
{
  if(instance == 1 && pin == GPIO_PA(9))
    return 1;
  if(instance == 1 && pin == GPIO_PB(6))
    return 0;
  if(instance == 2 && pin == GPIO_PA(2))
    return 1;
  if(instance == 2 && pin == GPIO_PA(14))
    return 1;

  extern int __invalid_stm32g0_uart_tx();
  return __invalid_stm32g0_uart_tx();
}


int stm32g0_uart_rx(int instance, gpio_t pin)
{
  if(instance == 1 && pin == GPIO_PA(10))
    return 1;
  if(instance == 1 && pin == GPIO_PB(7))
    return 0;
  if(instance == 2 && pin == GPIO_PA(3))
    return 1;
  if(instance == 2 && pin == GPIO_PA(15))
    return 1;

  extern int __invalid_stm32g0_uart_rx();
  return __invalid_stm32g0_uart_rx();
}


static stm32_uart_t *uarts[2];

void irq_27(void) { uart_irq(uarts[0]); }
void irq_28(void) { uart_irq(uarts[1]); }



stream_t *
stm32g0_uart_init(stm32_uart_t *u, unsigned int instance, int baudrate,
                  gpio_t tx, gpio_t rx, uint8_t flags)
{
  const stm32g0_uart_cfg_t *cfg = stm32g0_uart_config_get(instance);
  const int tx_af = stm32g0_uart_tx(instance, tx);
  const int rx_af = stm32g0_uart_rx(instance, rx);

  if(flags & UART_HALF_DUPLEX) {
    gpio_conf_af(tx, tx_af, GPIO_OPEN_DRAIN, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  } else {
    gpio_conf_af(tx, tx_af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_UP);
    gpio_conf_af(rx, rx_af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_UP);
  }

  u = stm32_uart_init(u,
                      (cfg->base << 8) + 0x40000000,
                      baudrate,
                      cfg->clkid,
                      cfg->irq,
                      flags,
                      0);

  if(flags & UART_HALF_DUPLEX)
    reg_wr(u->reg_base + USART_CR3, 0x8); // HDSEL

  uarts[instance - 1] = u;

  return &u->stream;
}
