#pragma once

#include <stdint.h>
#include <stdarg.h>

#define SYS_yield      0
#define SYS_relinquish 1
#define SYS_task_start 2
#define SYS_sleep      3



inline void
sys_isb(void)
{
  asm volatile ("isb\n\t");
}

inline void
sys_forbid()
{
  asm volatile ("cpsid i\n\t");
}

inline void
sys_permit()
{
    asm volatile ("cpsie i\n\t"
                  "isb\n\t");
}


inline void *
sys_get_psp()
{
  void *result;
  asm volatile ("mrs %0, psp\n\t" : "=r" (result));
  return result;
}


inline void
sys_set_psp(void *psp)
{
  asm volatile ("msr psp, %0\n\t" : : "r" (psp));
}

inline uint32_t
sys_get_control(void)
{
  uint32_t result;
  asm volatile ("mrs %0, control\n\t" : "=r" (result));
  return result;
}

inline void
sys_set_control(uint32_t val)
{
  asm volatile ("msr control, %0\n\t" : : "r" (val));
}




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


