#pragma once

/*
 * x86-64 half of the raw Linux syscall interface (see linux.h):
 * the syscall numbers (arch/x86/entry/syscalls/syscall_64.tbl), the
 * syscall instruction sequence and the signal frame layout.
 */

// ---- Syscall numbers ----

#define SYS_read              0
#define SYS_write             1
#define SYS_open              2
#define SYS_close             3
#define SYS_mmap              9
#define SYS_mprotect         10
#define SYS_munmap           11
#define SYS_rt_sigaction     13
#define SYS_rt_sigprocmask   14
#define SYS_rt_sigreturn     15
#define SYS_ioctl            16
#define SYS_writev           20
#define SYS_dup2             33
#define SYS_nanosleep        35
#define SYS_getpid           39
#define SYS_socket           41
#define SYS_connect          42
#define SYS_socketpair       53
#define SYS_clone            56
#define SYS_fork             57
#define SYS_execve           59
#define SYS_exit             60
#define SYS_fcntl            72
#define SYS_rt_sigsuspend   130
#define SYS_sigaltstack     131
#define SYS_gettid          186
#define SYS_futex           202
#define SYS_timer_create    222
#define SYS_timer_settime   223
#define SYS_clock_gettime   228
#define SYS_exit_group      231
#define SYS_tgkill          234
#define SYS_ppoll           271
#define SYS_getrandom       318

// ---- Syscall entry ----

static inline long
linux_syscall6(long n, long a, long b, long c, long d, long e, long f)
{
  register long r10 asm("r10") = d;
  register long r8 asm("r8") = e;
  register long r9 asm("r9") = f;
  long ret;
  asm volatile ("syscall"
                : "=a"(ret)
                : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                : "rcx", "r11", "memory");
  return ret;
}

// ---- Signal frame (arch/x86/include/uapi/asm/{sigcontext,ucontext}.h) ----

struct linux_sigcontext {
  unsigned long r8, r9, r10, r11, r12, r13, r14, r15;
  unsigned long rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
  unsigned short cs, gs, fs, ss;
  unsigned long err, trapno, oldmask, cr2;
  void *fpstate;
  unsigned long reserved[8];
};

struct linux_ucontext {
  unsigned long uc_flags;
  struct linux_ucontext *uc_link;
  struct linux_stack uc_stack;
  struct linux_sigcontext uc_mcontext;
  linux_sigset_t uc_sigmask;
};

static inline uintptr_t
linux_uc_pc(const struct linux_ucontext *uc)
{
  return uc->uc_mcontext.rip;
}

static inline uintptr_t
linux_uc_sp(const struct linux_ucontext *uc)
{
  return uc->uc_mcontext.rsp;
}
