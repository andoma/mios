#pragma once

#include <mios/io.h>

struct stream;

// base:     UARTE instance base address (secure, e.g. 0x500C6000 for UARTE20)
// irq:      peripheral IRQ number (e.g. 198 for SERIAL20)
struct stream *nrf54l_uart_init(uint32_t base, int irq, int baudrate,
                                gpio_t txpin, gpio_t rxpin, int flags);

#define UART_CTRLD_IS_PANIC 0x80
