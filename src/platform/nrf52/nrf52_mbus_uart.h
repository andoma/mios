#pragma once

#include <mios/io.h>

void nrf52_mbus_uart_init(gpio_t txpin, gpio_t rxpin, gpio_t txe, gpio_t rxe);

