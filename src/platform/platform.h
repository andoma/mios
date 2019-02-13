#pragma once

void *platform_heap_end(void);

// Early TX-only non-interrupt driven console
void platform_console_init_early(void);

void platform_init(void);

