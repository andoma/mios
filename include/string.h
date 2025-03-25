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

const char *strtbl(const char *str, size_t index);

__attribute__((access(write_only, 1), access(read_only, 2)))
char *strcpy(char *dst, const char *src);

__attribute__((access(read_only, 1), access(read_only, 2)))
size_t strspn(const char *s, const char *accept);

__attribute__((access(read_only, 1), access(read_only, 2)))
size_t strcspn(const char *s, const char *reject);

__attribute__((access(read_only, 1)))
char *strchr(const char *s, int c);

__attribute__((access(read_only, 1)))
char *strdup(const char *src);

__attribute__((access(write_only, 1), access(read_only, 2)))
void bin2hex(char *s, const void *data, size_t len);

__attribute__((access(read_only, 1), access(read_only, 2)))
int glob(const char *str, const char *pat);
