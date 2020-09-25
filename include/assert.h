#pragma once

void __assert_func(const char *expr, const char *file, int line)
  __attribute__((noreturn));

#ifndef NDEBUG
#define assert(EX) (void)((EX) || (__assert_func (#EX, __FILE__, __LINE__),0))
#else
#define assert(EX)
#endif
