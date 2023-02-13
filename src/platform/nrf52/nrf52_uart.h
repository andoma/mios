#pragma once

struct stream;

struct stream *nrf52_uart_init(int baudrate, gpio_t txpin, gpio_t rxpin,
                               int flags);

#define UART_CTRLD_IS_PANIC 0x80
