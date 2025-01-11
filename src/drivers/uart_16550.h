#pragma once

struct stream;

struct stream *uart_16550_create(uint32_t base_addr, int irq);

