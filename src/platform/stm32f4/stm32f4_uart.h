#include <mios/io.h>
#include <mios/stream.h>

#include "platform/stm32/stm32_uart.h"

stream_t *stm32f4_uart_init(struct stm32_uart *uart,
                            int instance, int baudrate,
                            gpio_t tx, gpio_t rx, uint8_t flags);
