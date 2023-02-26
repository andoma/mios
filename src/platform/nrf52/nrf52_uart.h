#pragma once

#define UART_BASE 0x40002000

#define UART_INTENSET (UART_BASE + 0x304)
#define UART_ENABLE   (UART_BASE + 0x500)
#define UART_PSELTXD  (UART_BASE + 0x50c)
#define UART_PSELRXD  (UART_BASE + 0x514)
#define UART_RXD      (UART_BASE + 0x518)
#define UART_TXD      (UART_BASE + 0x51c)
#define UART_BAUDRATE (UART_BASE + 0x524)

#define UART_TX_TASK  (UART_BASE + 0x8)
#define UART_TX_RDY   (UART_BASE + 0x11c)

#define UART_RX_TASK  (UART_BASE + 0x0)
#define UART_RX_RDY   (UART_BASE + 0x108)


struct stream;

struct stream *nrf52_uart_init(int baudrate, gpio_t txpin, gpio_t rxpin,
                               int flags);

#define UART_CTRLD_IS_PANIC 0x80
