#include <string.h>
#include "cpu.h"

void *
cpu_stack_init(uint64_t *stack, void *(*entry)(void *arg), void *arg,
               void (*thread_exit)(void))
{
  stack -= 32;
  memset(stack, 0, 8 * 32);
  stack[0] = (uint64_t)entry;
  stack[1] = (uint64_t)thread_exit;
  stack[10] = (uint64_t)arg;
  return stack;
}
