#pragma once

#include <mios/stream.h>
#include <mios/task.h>
#include <mios/device.h>

#include "stm32_dma.h"

#define TX_FIFO_SIZE 64
#define RX_FIFO_SIZE 32

typedef struct stm32_uart_stream {
  stream_t stream;

  device_t device;

  uint32_t reg_base;

  task_waitable_t wait_rx;
  task_waitable_t wait_tx;

  stm32_dma_instance_t tx_dma;

  uint8_t flags;

  gpio_t tx_enable;
  uint8_t tx_enabled;

  uint8_t rx_fifo_rdptr;
  uint8_t rx_fifo_wrptr;
  uint8_t tx_fifo_rdptr;
  uint8_t tx_fifo_wrptr;
  uint8_t tx_busy;

  uint8_t tx_fifo[TX_FIFO_SIZE];
  uint8_t rx_fifo[RX_FIFO_SIZE];

  uint32_t rx_overrun;
  uint32_t rx_noise;
  uint32_t rx_framing_error;

} stm32_uart_stream_t;


#define UART_WAKEUP         0x4

#define UART_CTRLD_IS_PANIC 0x80
#define UART_2_STOP_BITS    0x01