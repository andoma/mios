#pragma once

#include <mios/stream.h>
#include <mios/task.h>

#include "stm32_dma.h"

#define TX_FIFO_SIZE 64
#define RX_FIFO_SIZE 32

typedef struct stm32_uart_stream {
  stream_t stream;

  uint32_t reg_base;

  task_waitable_t wait_rx;
  task_waitable_t wait_tx;

  stm32_dma_instance_t tx_dma;

  uint8_t flags;

  uint8_t rx_fifo_rdptr;
  uint8_t rx_fifo_wrptr;
  uint8_t tx_fifo_rdptr;
  uint8_t tx_fifo_wrptr;
  uint8_t tx_busy;

  uint8_t tx_fifo[TX_FIFO_SIZE];
  uint8_t rx_fifo[RX_FIFO_SIZE];

} stm32_uart_stream_t;


#define UART_HALF_DUPLEX    0x1
#define UART_TXDMA          0x2
#define UART_WAKEUP         0x4

#define UART_CTRLD_IS_PANIC 0x80
