#pragma once

#include <stddef.h>
#include <stdarg.h>

int printf(const char *format, ...);

int vprintf(const char *format, va_list ap);

int snprintf(char *str, size_t size, const char *format, ...);

int getchar(void);

void init_printf(void *arg, void (*cb)(void *arg, char c));

void init_getchar(void *arg, int (*cb)(void *arg));

