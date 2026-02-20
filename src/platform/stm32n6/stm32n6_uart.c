#include "stm32n6_uart.h"
#include "stm32n6_clk.h"

#include <mios/type_macros.h>

#include "irq.h"


#define USART_CR1  0x00
#define USART_CR2  0x04
#define USART_CR3  0x08
#define USART_BRR  0x0c
#define USART_SR   0x1c
#define USART_ICR  0x20
#define USART_RDR  0x24
#define USART_TDR  0x28


#define USART_CR1_UE     (1 << 0)
#define USART_CR1_UESM   (1 << 1)
#define USART_CR1_RE     (1 << 2)
#define USART_CR1_TE     (1 << 3)
#define USART_CR1_RXNEIE (1 << 5)
#define USART_CR1_TCIE   (1 << 6)
#define USART_CR1_TXEIE  (1 << 7)

#define USART_SR_BUSY    (1 << 16)

#include "platform/stm32/stm32_uart_stream.c"


static const struct {
  uint32_t base;
  uint16_t clkid;
  uint8_t irq;
  uint8_t af;
} uart_config[] = {
  { 0x52001000, CLK_USART1, 159, 7},
  { 0x50004400, CLK_USART2, 160, 7},
  { 0x50004800, CLK_USART3, 161, 7},
  { 0x50004C00, CLK_USART4, 162, 8},
  { 0x50005000, CLK_USART5, 163, 11},
  { 0x52001400, CLK_USART6, 164, 7},
};


static stm32_uart_stream_t *uarts[6];

stream_t *
stm32n6_uart_init(stm32_uart_stream_t *u, unsigned int instance, int baudrate,
                  gpio_t tx, gpio_t rx, uint16_t flags, const char *name)
{
  instance--;
  if(instance >= ARRAYSIZE(uart_config))
    return NULL;

  const int af = uart_config[instance].af;

  const gpio_output_type_t ot = flags & UART_TX_OPEN_DRAIN ?
    GPIO_OPEN_DRAIN : GPIO_PUSH_PULL;
  gpio_conf_af(tx, af, ot, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  gpio_conf_af(rx, af, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);


  u = stm32_uart_stream_init(u,
                             uart_config[instance].base,
                             baudrate,
                             uart_config[instance].clkid,
                             uart_config[instance].irq,
                             flags,
                             name);
  uarts[instance] = u;
  return &u->stream;
}

void irq_159(void) { uart_irq(uarts[0]); }
void irq_160(void) { uart_irq(uarts[1]); }
void irq_161(void) { uart_irq(uarts[2]); }
void irq_162(void) { uart_irq(uarts[3]); }
void irq_163(void) { uart_irq(uarts[4]); }
void irq_164(void) { uart_irq(uarts[5]); }
