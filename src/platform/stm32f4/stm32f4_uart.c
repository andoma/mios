#include "stm32f4_clk.h"
#include "stm32f4_uart.h"

#define USART_SR    0x00
#define USART_TDR   0x04
#define USART_RDR   0x04
#define USART_BBR   0x08
#define USART_CR1   0x0c

#define CR1_IDLE       (1 << 13) | (1 << 5) | (1 << 3) | (1 << 2)
#define CR1_ENABLE_TXI CR1_IDLE | (1 << 7)


#include "platform/stm32/stm32_uart.c"




static const struct {
  uint16_t base;
  uint16_t clkid;
  uint8_t irq;
  uint8_t af;
} uart_config[] = {
  { 0x0110, CLK_USART1, 37, 7},
  { 0x0044, CLK_USART2, 38, 7},
  { 0x0048, CLK_USART3, 39, 7},
  { 0x004c, CLK_UART4,  52, 8},
  { 0x0050, CLK_UART5,  53, 8},
  { 0x0114, CLK_USART6, 71, 8},
};


static stm32_uart_t *uarts[6];

stream_t *
stm32f4_uart_init(stm32_uart_t *u, int instance, int baudrate,
                  gpio_t tx, gpio_t rx, uint8_t flags)
{
  if(instance < 1 || instance > 6)
    return NULL;

  instance--;

  const int af = uart_config[instance].af;
  gpio_conf_af(tx, af, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(rx, af, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);

  u = stm32_uart_init(u,
                      (uart_config[instance].base << 8) + 0x40000000,
                      baudrate,
                      uart_config[instance].clkid,
                      uart_config[instance].irq,
                      flags);

  uarts[instance] = u;

  return &u->stream;
}


void irq_37(void) { uart_irq(uarts[0]); }
void irq_38(void) { uart_irq(uarts[1]); }
void irq_39(void) { uart_irq(uarts[2]); }
void irq_52(void) { uart_irq(uarts[3]); }
void irq_53(void) { uart_irq(uarts[4]); }
void irq_71(void) { uart_irq(uarts[5]); }
