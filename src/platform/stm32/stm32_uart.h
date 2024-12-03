#pragma once

#define UART_SR_PARITY_ERR    (1 << 0)
#define UART_SR_FRAMING_ERR   (1 << 1)
#define UART_SR_NOISE_ERR     (1 << 2)
#define UART_SR_OVERRUN_ERR   (1 << 3)
#define UART_SR_IDLE          (1 << 4)
#define UART_SR_RXNE          (1 << 5)
#define UART_SR_TC            (1 << 6)
#define UART_SR_TXE           (1 << 7)
#define UART_SR_LBD           (1 << 8)
#define UART_SR_CTS           (1 << 9)
