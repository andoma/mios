#include <mios/io.h>
#include <mios/stream.h>

#include "platform/stm32/stm32_uart_stream.h"

stream_t *stm32h7_uart_init(stm32_uart_stream_t *u, unsigned int instance,
                            int baudrate,
                            gpio_t tx, gpio_t rx, uint8_t flags,
                            const char *name);

