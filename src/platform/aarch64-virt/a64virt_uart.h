#pragma once

#include <mios/io.h>

struct stream;

struct stream *a64virt_uart_init(int baudrate, gpio_t txpin, gpio_t rxpin,
                                 int flags);
