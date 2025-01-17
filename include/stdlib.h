#pragma once

#include <stddef.h>

void *malloc(size_t size) __attribute__((malloc,warn_unused_result));

void *calloc(size_t nmemb, size_t size) __attribute__((malloc,warn_unused_result));

void free(void *ptr);

void *memalign(size_t size, size_t alignment) __attribute__((malloc,warn_unused_result));

int atoi(const char *s);

unsigned int xtoi(const char *s);

unsigned int atoix(const char *s); // Uses xtoi() if string starts with 0x

#define RAND_MAX 0x7fffffff

int rand(void);
