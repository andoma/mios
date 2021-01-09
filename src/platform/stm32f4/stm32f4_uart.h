#include <mios/task.h>
#include <mios/io.h>
#include <mios/stream.h>

#define TX_FIFO_SIZE 32
#define RX_FIFO_SIZE 32

typedef struct {
  stream_t stream;

  uint32_t reg_base;

  task_waitable_t wait_rx;
  task_waitable_t wait_tx;

  uint8_t flags;

  uint8_t rx_fifo_rdptr;
  uint8_t rx_fifo_wrptr;
  uint8_t tx_fifo_rdptr;
  uint8_t tx_fifo_wrptr;
  uint8_t tx_busy;

  uint8_t tx_fifo[TX_FIFO_SIZE];
  uint8_t rx_fifo[RX_FIFO_SIZE];

} stm32f4_uart_t;


#define UART_CTRLD_IS_PANIC 0x80

stream_t *stm32f4_uart_init(stm32f4_uart_t *uart, int instance, int baudrate,
                            gpio_t tx, gpio_t rx, uint8_t flags);
