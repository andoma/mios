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
  uint8_t tx_pol_invert;
  uint8_t tx_enabled;

  gpio_t rx_enable;
  uint8_t rx_pol_invert;

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
  uint32_t rx_fifo_overrun;

} stm32_uart_stream_t;



// -- Flags passed to uart_init functions

// STOP-bit config bits are written as-is to register, don't change
// withuot adjusting corresponding setup code
#define UART_1_STOP_BIT     0x00
#define UART_0_5_STOP_BITS  0x01
#define UART_2_STOP_BITS    0x02
#define UART_1_5_STOP_BITS  0x03

#define UART_WAKEUP         0x4   // Enables wakeup from sleep

#define UART_HALF_DUPLEX    0x8   // Run UART in halfduplex mode

#define UART_DEBUG          0x40

#define UART_CTRLD_IS_PANIC 0x80 /* panic system if CTRL-D is receviced
                                  * useful on system console
                                  */
