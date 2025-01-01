#include <stdint.h>
#include <mios/task.h>

#include "cpu.h"


struct cpu cpu0;


void
reboot(void)
{

}


void *
cpu_stack_init(uint64_t *stack, void *(*entry)(void *arg), void *arg,
               void (*thread_exit)(void *))
{
  return stack;
}
