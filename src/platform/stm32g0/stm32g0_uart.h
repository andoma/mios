#pragma once

#include <mios/io.h>
#include <mios/stream.h>

#define USART_CR1  0x00
#define USART_CR2  0x04
#define USART_CR3  0x08
#define USART_BRR  0x0c
#define USART_SR   0x1c
#define USART_ICR  0x20
#define USART_RDR  0x24
#define USART_TDR  0x28


#define USART_CR1_UE     (1 << 0)
#define USART_CR1_RE     (1 << 2)
#define USART_CR1_TE     (1 << 3)
#define USART_CR1_RXNEIE (1 << 5)
#define USART_CR1_TCIE   (1 << 6)
#define USART_CR1_TXEIE  (1 << 7)

#define USART_SR_BUSY    (1 << 16)

#include "platform/stm32/stm32_uart.h"

typedef struct {
  uint16_t base;
  uint16_t clkid;
  uint8_t irq;
  uint8_t txdma;
  uint8_t rxdma;
} stm32g0_uart_cfg_t;

const stm32g0_uart_cfg_t *stm32g0_uart_config_get(int instance);

int stm32g0_uart_tx(int instance, gpio_t pin);

int stm32g0_uart_rx(int instance, gpio_t pin);

stream_t *stm32g0_uart_init(stm32_uart_t *u, unsigned int instance,
                            int baudrate, gpio_t tx, gpio_t rex,
                            uint8_t flags);

void stm32g0_mbus_uart_create(unsigned int instance, int baudrate,
                              gpio_t tx, gpio_t rx, gpio_t txe,
                              uint8_t prio, int flags);
