#include <string.h>
#include "cpu.h"


/**
 * context as saved on stack:
 *
 *  16 caller saved (Always stored on trap entry)
 *
 * 29  x31      t6
 * 28  x30      t5
 * 27  x29      t4
 * 26  x28      t3
 * 25  x17      a7
 * 24  x16
 * 23  x15
 * 22  x14
 * 21  x13
 * 20  x12
 * 19  x11
 * 18  x10      a0
 * 17  x7       t2
 * 16  x6       t1
 * 15  x5       t0
 * 14  x1       ra
 *
 * 14 callee saved (Saved on context switch only)
 *
 * 13  x27      s11
 * 12  x26      s10
 * 11  x25      s9
 * 10  x24      s8
 * 9   x23      s7
 * 8   x22      s6
 * 7   x21      s5
 * 6   x20      s4
 * 5   x19      s3
 * 4   x18      s2
 * 3   x9       s1
 * 2   x8       s0
 * 1   x4       tp
 * 0   pc       pc
 */




void *
cpu_stack_init(uint64_t *stack, void *(*entry)(void *arg), void *arg,
               void (*thread_exit)(void))
{
  stack -= 16 + 14;
  memset(stack, 0, 8 * 30);
  stack[0]  = (uint64_t)entry;
  stack[14] = (uint64_t)thread_exit;
  stack[18] = (uint64_t)arg;
  return stack;
}

static cpu_t cpu0;

void
cpu_init(void)
{
  strlcpy(cpu0.name, "cpu0", sizeof(cpu0.name));
  task_init_cpu(&cpu0.sched, cpu0.name);

  asm volatile ("csrw mscratch, %0\n\t" :: "r" (&cpu0));
}
