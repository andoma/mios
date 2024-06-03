#pragma once

struct stream;

struct stream *pl011_uart_init(uint32_t base, int baudrate, int irq);
