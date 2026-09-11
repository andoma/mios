#pragma once

/*
 * AArch64 half of the raw Linux syscall interface (see linux.h):
 * the syscall numbers (the generic table, include/uapi/asm-generic/
 * unistd.h), the syscall instruction sequence and the signal frame
 * layout.
 *
 * The generic table has no open/dup2/fork; linux.h builds those out of
 * openat/dup3/clone.
 */

// ---- Syscall numbers ----

#define SYS_dup3             24
#define SYS_ioctl            29
#define SYS_fcntl            25
#define SYS_openat           56
#define SYS_close            57
#define SYS_read             63
#define SYS_write            64
#define SYS_writev           66
#define SYS_ppoll            73
#define SYS_futex            98
#define SYS_nanosleep       101
#define SYS_timer_create    107
#define SYS_timer_settime   110
#define SYS_clock_gettime   113
#define SYS_tgkill          131
#define SYS_sigaltstack     132
#define SYS_rt_sigsuspend   133
#define SYS_rt_sigaction    134
#define SYS_rt_sigprocmask  135
#define SYS_rt_sigreturn    139
#define SYS_getpid          172
#define SYS_gettid          178
#define SYS_socket          198
#define SYS_socketpair      199
#define SYS_connect         203
#define SYS_munmap          215
#define SYS_clone           220
#define SYS_execve          221
#define SYS_mmap            222
#define SYS_mprotect        226
#define SYS_exit             93
#define SYS_exit_group       94
#define SYS_getrandom       278

// ---- Syscall entry ----

static inline long
linux_syscall6(long n, long a, long b, long c, long d, long e, long f)
{
  register long x8 asm("x8") = n;
  register long x0 asm("x0") = a;
  register long x1 asm("x1") = b;
  register long x2 asm("x2") = c;
  register long x3 asm("x3") = d;
  register long x4 asm("x4") = e;
  register long x5 asm("x5") = f;
  asm volatile ("svc #0"
                : "+r"(x0)
                : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                : "memory");
  return x0;
}

// ---- Signal frame (arch/arm64/include/uapi/asm/{sigcontext,ucontext}.h) ----

struct linux_sigcontext {
  unsigned long fault_address;
  unsigned long regs[31];       // x0 - x30
  unsigned long sp;
  unsigned long pc;
  unsigned long pstate;
  // FP/SIMD (and SVE, ...) state as a chain of tagged records
  unsigned char __reserved[4096] __attribute__((aligned(16)));
};

struct linux_ucontext {
  unsigned long uc_flags;
  struct linux_ucontext *uc_link;
  struct linux_stack uc_stack;
  linux_sigset_t uc_sigmask;
  // The kernel pads uc_sigmask out to the 1024 bits glibc uses
  unsigned char __unused[1024 / 8 - sizeof(linux_sigset_t)];
  struct linux_sigcontext uc_mcontext;
};

_Static_assert(__builtin_offsetof(struct linux_ucontext, uc_mcontext) == 176,
               "ucontext layout");

static inline uintptr_t
linux_uc_pc(const struct linux_ucontext *uc)
{
  return uc->uc_mcontext.pc;
}

static inline uintptr_t
linux_uc_sp(const struct linux_ucontext *uc)
{
  return uc->uc_mcontext.sp;
}
