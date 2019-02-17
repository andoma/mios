#pragma once

#define NULL ((void *)0)

#define offsetof(st, m) __builtin_offsetof(st, m)

typedef __SIZE_TYPE__ size_t;

typedef long signed int ssize_t;
