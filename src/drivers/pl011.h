#pragma once

struct stream;

struct stream *pl011_uart_init(long base, int baudrate, int irq);
