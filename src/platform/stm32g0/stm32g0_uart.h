#include <mios/io.h>
#include <mios/stream.h>

#include "platform/stm32/stm32_uart.h"
#include "platform/stm32/stm32_tim.h"


stream_t *stm32g0_uart_init(stm32_uart_t *u, unsigned int instance,
                            int baudrate,
                            gpio_af_t tx, gpio_af_t rx, uint8_t flags);

void stm32g0_mbus_uart_create(unsigned int instance, int baudrate,
                              gpio_af_t tx, gpio_af_t rx, gpio_t txe,
                              uint8_t local_addr,
                              const stm32_timer_info_t *timer,
                              uint8_t prio, int flags);


static inline gpio_af_t stm32g0_uart_tx(int instance, gpio_t pin)
{
  if(instance == 1 && pin == GPIO_PA(9)) {
    return (gpio_af_t){.gpio = pin, .af = 1};
  } else if(instance == 1 && pin == GPIO_PB(6)) {
    return (gpio_af_t){.gpio = pin, .af = 0};
  } else if(instance == 2 && pin == GPIO_PA(2)) {
    return (gpio_af_t){.gpio = pin, .af = 1};
  } else if(instance == 2 && pin == GPIO_PA(14)) {
    return (gpio_af_t){.gpio = pin, .af = 1};
  }

  extern void invalid_uart_tx_pin(void);
  invalid_uart_tx_pin();
  return (gpio_af_t){};
}


static inline gpio_af_t stm32g0_uart_rx(int instance, gpio_t pin)
{
  if(instance == 1 && pin == GPIO_PA(10)) {
    return (gpio_af_t){.gpio = pin, .af = 1};
  } else if(instance == 1 && pin == GPIO_PB(7)) {
    return (gpio_af_t){.gpio = pin, .af = 0};
  } else if(instance == 2 && pin == GPIO_PA(3)) {
    return (gpio_af_t){.gpio = pin, .af = 1};
  } else if(instance == 2 && pin == GPIO_PA(15)) {
    return (gpio_af_t){.gpio = pin, .af = 1};
  }

  extern void invalid_uart_rx_pin(void);
  invalid_uart_rx_pin();
  return (gpio_af_t){};
}
