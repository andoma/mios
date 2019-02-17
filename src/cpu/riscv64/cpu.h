#pragma once

#include <stdint.h>

#define MIN_STACK_SIZE 4096

void *cpu_stack_init(uint64_t *stack, void *(*entry)(void *arg), void *arg,
                     void (*thread_exit)(void));
