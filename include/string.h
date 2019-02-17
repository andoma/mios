#pragma once

#include <stddef.h>

int memcmp(const void *s1, const void *s2, size_t n);

void *memcpy(void *dest, const void *src, size_t n);

void *memmove(void *dest, const void *src, size_t n);

void *memset(void *s, int c, size_t n);

size_t strlen(const char *s);

char *strncpy(char *dest, const char *src, size_t n);

size_t strlcpy(char * restrict dst, const char * restrict src, size_t siz);
