#include "stm32f4_uart.h"

#include "stm32f4_clk.h"
#include "stm32f4_dma.h"
#include "stm32f4_tim.h"
#include "platform/stm32/stm32_uart_stream.c"



static const stm32f4_uart_config_t stm32f4_uart_config[] = {
  { 0x0110, CLK_USART1, 37, 7, STM32F4_DMA_USART1_TX, STM32F4_DMA_USART1_RX},
  { 0x0044, CLK_USART2, 38, 7, STM32F4_DMA_USART2_TX, STM32F4_DMA_USART2_RX},
  { 0x0048, CLK_USART3, 39, 7, STM32F4_DMA_USART3_TX, STM32F4_DMA_USART3_RX},
  { 0x004c, CLK_UART4,  52, 8, STM32F4_DMA_UART4_TX,  STM32F4_DMA_UART4_RX},
  { 0x0050, CLK_UART5,  53, 8, STM32F4_DMA_UART5_TX,  STM32F4_DMA_UART5_RX},
  { 0x0114, CLK_USART6, 71, 8, STM32F4_DMA_USART6_TX, STM32F4_DMA_USART6_RX}
};

const stm32f4_uart_config_t *
stm32f4_uart_get_config(int index)
{
  return &stm32f4_uart_config[index];
}

static stm32_uart_stream_t *uarts[6];

stream_t *
stm32f4_uart_stream_init(stm32_uart_stream_t *u, int instance, int baudrate,
                         gpio_t tx, gpio_t rx, gpio_t tx_enable, uint8_t flags,
                         const char *name)
{
  const int index = instance - 1;
  const stm32f4_uart_config_t *cfg = stm32f4_uart_get_config(index);

  const int af = cfg->af;
  gpio_conf_af(tx, af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(rx, af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_UP);

  u = stm32_uart_stream_init(u,
                             (cfg->base << 8) + 0x40000000,
                             baudrate,
                             cfg->clkid,
                             cfg->irq,
                             flags,
                             tx_enable,
                             name);

  uarts[index] = u;

  return &u->stream;
}


void irq_37(void) { uart_irq(uarts[0]); }
void irq_38(void) { uart_irq(uarts[1]); }
void irq_39(void) { uart_irq(uarts[2]); }
void irq_52(void) { uart_irq(uarts[3]); }
void irq_53(void) { uart_irq(uarts[4]); }
void irq_71(void) { uart_irq(uarts[5]); }

