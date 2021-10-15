#pragma once

void __assert_func(const char *expr, const char *file, int line)
  __attribute__((noreturn));

#ifndef NDEBUG
#define assert(EX) (void)((__builtin_expect(EX, 1)) || (__assert_func (#EX, __FILE__, __LINE__),0))
#else
#define assert(EX)
#endif
