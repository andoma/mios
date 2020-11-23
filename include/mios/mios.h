#pragma once

void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#define ARRAYSIZE(x) (sizeof(x) / sizeof(x[0]))

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
