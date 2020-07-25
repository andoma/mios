#pragma once

void panic(const char *fmt, ...) __attribute__((noreturn));

void sleephz(int ticks);

#define ARRAYSIZE(x) (sizeof(x) / sizeof(x[0]))
