#pragma once

#include <stdint.h>
#include <stdarg.h>

#define SYS_yield      0
#define SYS_relinquish 1
#define SYS_task_start 2
#define SYS_sleep      3

inline int
syscall0(int syscall)
{
  register uint32_t r12 asm("r12") = syscall;
  register uint32_t result asm("r0");
  asm volatile ("svc #0" : "=r" (result) : "r"(r12));
  return result;
}

inline int
syscall1(int syscall, int arg1)
{
  register uint32_t r12 asm("r12") = syscall;
  register uint32_t r0 asm("r0") = arg1;
  register uint32_t result asm("r0");
  asm volatile ("svc #0" : "=r" (result) : "r"(r12), "r" (r0));
  return result;
}


inline void
sys_schedule(void)
{
  volatile unsigned int * const ICSR = (unsigned int *)0xe000ed04;
  *ICSR = 1 << 28; // Raise PendSV interrupt
}

