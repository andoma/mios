#include "stm32g0_clk.h"
#include "stm32g0_uart.h"
#include "stm32g0_tim.h"

#ifdef ENABLE_OTA
#include "stm32g0_ota.h"
#endif

#define USART_CR1  0x00
#define USART_CR2  0x04
#define USART_CR3  0x08
#define USART_BBR  0x0c
#define USART_SR   0x1c
#define USART_ICR  0x20
#define USART_RDR  0x24
#define USART_TDR  0x28

#define CR1_IDLE       (1 << 0) | (1 << 5) | (1 << 3) | (1 << 2)
#define CR1_ENABLE_TXI CR1_IDLE | (1 << 7)

#include "platform/stm32/stm32_uart.c"
#include "platform/stm32/stm32_mbus_uart.c"

static const struct {
  uint16_t base;
  uint16_t clkid;
  uint8_t irq;
} uart_config[] = {
  { 0x0138, CLK_USART1, 27},
  { 0x0044, CLK_USART2, 28},
};

static stm32_uart_t *uarts[2];

stream_t *
stm32g0_uart_init(stm32_uart_t *u, unsigned int instance, int baudrate,
                  gpio_af_t tx, gpio_af_t rx, uint8_t flags)
{
  instance--;

  if(instance > ARRAYSIZE(uart_config))
    return NULL;

  if(flags & UART_HALF_DUPLEX) {
    gpio_conf_af(tx.gpio, tx.af, GPIO_OPEN_DRAIN, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
  } else {
    gpio_conf_af(tx.gpio, tx.af, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_NONE);
    gpio_conf_af(rx.gpio, rx.af, GPIO_PUSH_PULL, GPIO_SPEED_HIGH, GPIO_PULL_UP);
  }

  u = stm32_uart_init(u,
                      (uart_config[instance].base << 8) + 0x40000000,
                      baudrate,
                      uart_config[instance].clkid,
                      uart_config[instance].irq,
                      flags,
                      0);

  if(flags & UART_HALF_DUPLEX)
    reg_wr(u->reg_base + USART_CR3, 0x8); // HDSEL

  uarts[instance] = u;

  return &u->stream;
}

void irq_27(void) { uart_irq(uarts[0]); }
void irq_28(void) { uart_irq(uarts[1]); }



void
stm32g0_mbus_uart_create(unsigned int instance, int baudrate,
                         gpio_af_t tx, gpio_af_t rx, gpio_t txe,
                         uint8_t local_addr,
                         const stm32_timer_info_t *timer,
                         uint8_t prio, int flags)
{
  instance--;

  if(instance > ARRAYSIZE(uart_config))
    return;

  gpio_conf_af(tx.gpio, tx.af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);
  gpio_conf_af(rx.gpio, rx.af, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_UP);
  if(txe != GPIO_UNUSED)
    gpio_conf_output(txe, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);


  unsigned int freq = clk_get_freq(uart_config[instance].clkid);

  if(flags & UART_WAKEUP) {
    // Run USART from HSI16 so it can resume us from STOP
    reg_set_bits(RCC_CCIPR, 2 * instance, 2, 2);
    freq = 16000000;
  }

  const unsigned int bbr = (freq + baudrate - 1) / baudrate;

  const uint32_t baseaddr = (uart_config[instance].base << 8) + 0x40000000;

#ifdef ENABLE_OTA
  if(flags & UART_MBUS_OTA) {
    stm32g0_ota_configure(baseaddr, local_addr, txe);
  }
#endif

  stm32_mbus_uart_create(baseaddr,
                         bbr,
                         uart_config[instance].clkid,
                         uart_config[instance].irq,
                         0, txe,
                         local_addr,
                         timer,
                         prio, flags);
}
