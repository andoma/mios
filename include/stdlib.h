#pragma once

#include <stddef.h>

void *malloc(size_t size) __attribute__((malloc,warn_unused_result));

void *calloc(size_t nmemb, size_t size) __attribute__((malloc,warn_unused_result));

void free(void *ptr);

void *memalign(size_t size, size_t alignment) __attribute__((malloc,warn_unused_result));
