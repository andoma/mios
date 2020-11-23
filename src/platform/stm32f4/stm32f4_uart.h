#include <mios/task.h>
#include <mios/io.h>

#define TX_FIFO_SIZE 32
#define RX_FIFO_SIZE 32

typedef struct {

  uint32_t reg_base;

  task_waitable_t wait_rx;
  task_waitable_t wait_tx;

  uint8_t rx_fifo_rdptr;
  uint8_t rx_fifo_wrptr;
  uint8_t tx_fifo_rdptr;
  uint8_t tx_fifo_wrptr;
  uint8_t tx_busy;

  uint8_t tx_fifo[TX_FIFO_SIZE];
  uint8_t rx_fifo[RX_FIFO_SIZE];

} stm32f4_uart_t;

void stm32f4_uart_init(stm32f4_uart_t *uart, int reg_base, int baudrate,
                       gpio_t tx, gpio_t rx);

void stm32f4_uart_putc(stm32f4_uart_t *uart, char c);

int stm32f4_uart_getc(stm32f4_uart_t *uart);

