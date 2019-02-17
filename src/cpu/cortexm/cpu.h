#pragma once

#include <stdint.h>

#define MIN_STACK_SIZE 256

void *cpu_stack_init(uint32_t *stack, void *(*entry)(void *arg), void *arg,
                     void (*thread_exit)(void));
