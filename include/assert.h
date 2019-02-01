#pragma once

void __assert_func(const char *expr, const char *file, int line);

#define assert(EX) (void)((EX) || (__assert_func (#EX, __FILE__, __LINE__),0))
