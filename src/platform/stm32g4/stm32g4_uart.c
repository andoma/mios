#include "stm32g4_clk.h"
#include "stm32g4_uart.h"

#define USART_CR1  0x00
#define USART_CR3  0x08
#define USART_BBR  0x0c
#define USART_SR   0x1c
#define USART_RDR  0x24
#define USART_TDR  0x28

#define CR1_IDLE       (1 << 0) | (1 << 5) | (1 << 3) | (1 << 2)
#define CR1_ENABLE_TXI CR1_IDLE | (1 << 7)

#include "platform/stm32/stm32_uart.c"

static const struct {
  uint16_t base;
  uint16_t clkid;
  uint8_t irq;
  uint8_t af;
} uart_config[] = {
  { 0x0138, CLK_USART1, 37, 7},
  { 0x0044, CLK_USART2, 38, 7},
  { 0x0048, CLK_USART3, 39, 7},
};

static stm32_uart_t *uarts[3];

stream_t *
stm32g4_uart_init(stm32_uart_t *u, unsigned int instance, int baudrate,
                  gpio_t tx, gpio_t rx, uint8_t flags)
{
  instance--;

  if(instance > ARRAYSIZE(uart_config))
    return NULL;

  const int af = uart_config[instance].af;

  if(flags & UART_HALF_DUPLEX) {
    gpio_conf_af(tx, af, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  } else {
    gpio_conf_af(tx, af, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
    gpio_conf_af(rx, af, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);
  }

  u = stm32_uart_init(u,
                      (uart_config[instance].base << 8) + 0x40000000,
                      baudrate,
                      uart_config[instance].clkid,
                      uart_config[instance].irq,
                      flags,
                      0);

  if(flags & UART_HALF_DUPLEX)
    reg_wr(u->reg_base + USART_CR3, 0x8); // HDSEL

  uarts[instance] = u;

  return &u->stream;
}

void irq_37(void) { uart_irq(uarts[0]); }
void irq_38(void) { uart_irq(uarts[1]); }
void irq_39(void) { uart_irq(uarts[2]); }
