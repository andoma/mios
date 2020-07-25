#pragma once

void panic(const char *fmt, ...) __attribute__((noreturn));

#define ARRAYSIZE(x) (sizeof(x) / sizeof(x[0]))
