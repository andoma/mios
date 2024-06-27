#include <mios/io.h>
#include <mios/stream.h>

#include "platform/stm32/stm32_uart_stream.h"

#define USART_SR    0x00
#define USART_TDR   0x04
#define USART_RDR   0x04
#define USART_BRR   0x08
#define USART_CR1   0x0c
#define USART_CR2   0x10
#define USART_CR3   0x14

#define USART_CR1_UE     (1 << 13)
#define USART_CR1_RE     (1 << 2)
#define USART_CR1_TE     (1 << 3)
#define USART_CR1_RXNEIE (1 << 5)
#define USART_CR1_TCIE   (1 << 6)
#define USART_CR1_TXEIE  (1 << 7)

typedef struct {
  uint16_t base;
  uint16_t clkid;
  uint8_t irq;
  uint8_t af;
  uint32_t txdma;
  uint32_t rxdma;
} stm32f4_uart_config_t;

stream_t *stm32f4_uart_stream_init(struct stm32_uart_stream *uart,
                                   int instance, int baudrate,
                                   gpio_t tx, gpio_t rx, uint8_t flags,
                                   const char *name);

void stm32f4_uart_mbus_multidrop_create(unsigned int instance,
                                        gpio_t tx, gpio_t rx, gpio_t txe,
                                        const char *name);

const stm32f4_uart_config_t *stm32f4_uart_get_config(int index);
