#pragma once

void *platform_heap_end(void);

// Early TX-only non-interrupt driven console
void platform_console_init_early(void);

// Regular console with TX / RX
void platform_console_init(void);

