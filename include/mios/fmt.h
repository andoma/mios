#pragma once

#include <stddef.h>
#include <stdarg.h>

typedef size_t (fmtcb_t)(void *aux, const char *s, size_t len);

size_t fmtv(fmtcb_t *cb, void *aux, const char *fmt, va_list ap);
