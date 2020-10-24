#pragma once

#include <stddef.h>
#include <stdarg.h>

int printf(const char *format, ...) __attribute__ ((format(printf, 1, 2)));

int vprintf(const char *format, va_list ap);

int snprintf(char *str, size_t size, const char *format, ...) __attribute__ ((format(printf, 3, 4)));

int getchar(void);

void init_printf(void *arg, void (*cb)(void *arg, char c));

void init_getchar(void *arg, int (*cb)(void *arg));

typedef size_t (fmtcb_t)(void *aux, const char *s, size_t len);

size_t fmtv(fmtcb_t *cb, void *aux, const char *fmt, va_list ap);
