#pragma once

struct stream;

struct stream *nrf52_uart_init(int baudrate, gpio_t txpin, gpio_t rxpin,
                               int flags);
