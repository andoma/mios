#include "nrf52_reg.h"
#include "nrf52_uart.h"

#define UART_WAKEUP 0

#include "lib/mbus/mbus_uart.c"


static void
mbus_uart_tx_byte(uart_mbus_t *um, uint8_t byte)
{
  reg_wr(UART_TXD, byte);
  reg_wr(UART_TX_TASK, 1);
}

static int
mbus_uart_is_busy(uart_mbus_t *um)
{
  return 0;
}


static void
nrf52_mbus_uart_irq(void *arg)
{
  uart_mbus_t *um = arg;

  if(reg_rd(UART_RX_RDY)) {
    reg_wr(UART_RX_RDY, 0);
    char c = reg_rd(UART_RXD);
    if(!fifo_is_full(&um->rxfifo)) {
      fifo_wr(&um->rxfifo, c);
      softirq_trig(&um->softirq);
    } else {
      um->rx_fifo_full++;
    }
  }
}




void
nrf52_mbus_uart_init(gpio_t txpin, gpio_t rxpin, gpio_t txe,
                     uint8_t local_addr, uint8_t prio, int flags)
{
  uart_mbus_t *um = calloc(1, sizeof(uart_mbus_t));

  gpio_conf_output(txe, GPIO_PUSH_PULL, GPIO_SPEED_LOW, GPIO_PULL_NONE);

  reg_wr(UART_PSELTXD, txpin);
  reg_wr(UART_PSELRXD, rxpin);
  reg_wr(UART_ENABLE, 4);
  reg_wr(UART_BAUDRATE, 0x1d60000);

  reg_wr(UART_INTENSET, 0x4); // RXDRDY
  reg_wr(UART_RX_TASK, 1);

  irq_enable_fn_arg(2, IRQ_LEVEL_CONSOLE, nrf52_mbus_uart_irq, um);

  mbus_uart_init_common(um, txe, local_addr, prio, flags);
}
