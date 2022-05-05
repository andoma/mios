#pragma once

#include <stddef.h>

__attribute__((access(read_only, 1, 3), access(read_only, 2, 3)))
int memcmp(const void *s1, const void *s2, size_t n);

__attribute__((access(write_only, 1, 3), access(read_only, 2, 3)))
void *memcpy(void *dest, const void *src, size_t n);

__attribute__((access(write_only, 1, 3), access(read_only, 2, 3)))
void *memmove(void *dest, const void *src, size_t n);

__attribute__((access(write_only, 1, 3)))
void *memset(void *s, int c, size_t n);

__attribute__((access(read_only, 1)))
size_t strlen(const char *s);

__attribute__((access(write_only, 1, 3)))
char *strncpy(char *dest, const char *src, size_t n);

__attribute__((access(write_only, 1, 3)))
size_t strlcpy(char * restrict dst, const char * restrict src, size_t siz);

__attribute__((access(read_only, 1), access(read_only, 2)))
int strcmp(const char *s1, const char *s2);
