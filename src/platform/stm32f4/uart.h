#pragma once

#include <task.h>

#define TX_FIFO_SIZE 128
#define RX_FIFO_SIZE 64


typedef struct {

  uint32_t reg_base;

  struct task_queue wait_rx;
  struct task_queue wait_tx;

  uint8_t rx_fifo_rdptr;
  uint8_t rx_fifo_wrptr;
  uint8_t tx_fifo_rdptr;
  uint8_t tx_fifo_wrptr;
  uint8_t tx_busy;

  uint8_t tx_fifo[TX_FIFO_SIZE];
  uint8_t rx_fifo[RX_FIFO_SIZE];

} uart_t;

void uart_init(uart_t *uart, int reg_base, int baudrate);

void uart_irq(uart_t *uart);

void uart_putc(void *uart, char c);

int uart_getc(void *uart);

