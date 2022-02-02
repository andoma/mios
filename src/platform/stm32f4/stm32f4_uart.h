#include <mios/io.h>
#include <mios/stream.h>

#include "platform/stm32/stm32_uart.h"
#include "platform/stm32/stm32_tim.h"

stream_t *stm32f4_uart_init(struct stm32_uart *uart,
                            int instance, int baudrate,
                            gpio_t tx, gpio_t rx, uint8_t flags);

void stm32f4_mbus_uart_create(unsigned int instance, int baudrate,
                              gpio_t tx, gpio_t rx, gpio_t txe,
                              uint8_t local_addr,
                              const stm32_timer_info_t *timer);
