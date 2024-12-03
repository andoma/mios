#include "stm32g4_clk.h"
#include "stm32g4_uart.h"

#include "platform/stm32/stm32_uart_stream.c"

static const stm32g4_uart_config_t uart_config[] = {
  { 0x0138, CLK_USART1, 37, 7, 25, 24},
  { 0x0044, CLK_USART2, 38, 7, 27, 26},
  { 0x0048, CLK_USART3, 39, 7, 29, 28},
};

static stm32_uart_stream_t *uarts[3];

const stm32g4_uart_config_t *
stm32g4_uart_get_config(int index)
{
  return &uart_config[index];
}

stream_t *
stm32g4_uart_stream_init(stm32_uart_stream_t *u, unsigned int instance,
                         int baudrate, gpio_t tx, gpio_t rx, uint8_t flags,
                         const char *name)
{
  instance--;

  if(instance > ARRAYSIZE(uart_config))
    return NULL;

  const int af = uart_config[instance].af;

  gpio_conf_af(tx, af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(rx, af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_UP);

  u = stm32_uart_stream_init(u,
                             (uart_config[instance].base << 8) + 0x40000000,
                             baudrate,
                             uart_config[instance].clkid,
                             uart_config[instance].irq,
                             flags,
                             name);

  uarts[instance] = u;

  return &u->stream;
}

void irq_37(void) { uart_irq(uarts[0]); }
void irq_38(void) { uart_irq(uarts[1]); }
void irq_39(void) { uart_irq(uarts[2]); }
