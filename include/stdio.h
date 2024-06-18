#pragma once

#include <stddef.h>
#include <stdarg.h>
#include <mios/stream.h>

int printf(const char *format, ...) __attribute__ ((format(printf, 1, 2)));

int vprintf(const char *format, va_list ap);

int snprintf(char *str, size_t size, const char *format, ...) __attribute__ ((format(printf, 3, 4)));

int vsnprintf(char *str, size_t size, const char *format, va_list ap);

int getchar(void);

extern stream_t *stdio;

int vstprintf(stream_t *s, const char *format, va_list ap);

int stprintf(stream_t *s, const char *format, ...) __attribute__ ((format(printf, 2, 3)));

void sthexdump(stream_t *s, const char *prefix, const void *data, size_t len,
               unsigned int offset);

void hexdump(const char *prefix, const void *data, size_t len);

void stprintflags(stream_t *s, const char *str,
                  unsigned int flags, const char *sep);

void sthexstr(stream_t *s, const void *data, size_t len);
