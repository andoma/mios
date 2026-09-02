#pragma once

/*
 * Raw Linux x86-64 syscall interface.
 *
 * Mios is built with -nostdinc and its own libc, so nothing from glibc
 * is available (or wanted: the symbol names collide). These are the
 * only syscalls the host port uses. Structures follow the kernel ABI
 * (uapi), not glibc's.
 */

#include <stdint.h>
#include <stddef.h>

#define SYS_read              0
#define SYS_write             1
#define SYS_open              2
#define SYS_close             3
#define SYS_mmap              9
#define SYS_munmap           11
#define SYS_rt_sigaction     13
#define SYS_rt_sigprocmask   14
#define SYS_rt_sigreturn     15
#define SYS_ioctl            16
#define SYS_nanosleep        35
#define SYS_getpid           39
#define SYS_execve           59
#define SYS_fcntl            72
#define SYS_rt_sigsuspend   130
#define SYS_sigaltstack     131
#define SYS_gettid          186
#define SYS_timer_create    222
#define SYS_timer_settime   223
#define SYS_clock_gettime   228
#define SYS_exit_group      231
#define SYS_tgkill          234
#define SYS_ppoll           271

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

#define linux_syscall(n, ...) \
  linux_syscall_(n, ##__VA_ARGS__, 0, 0, 0, 0, 0, 0)
#define linux_syscall_(n, a, b, c, d, e, f, ...) \
  linux_syscall6((long)(n), (long)(a), (long)(b), (long)(c), \
                 (long)(d), (long)(e), (long)(f))

// errno values we care about (returned negated)
#define LINUX_EINTR   4
#define LINUX_EAGAIN 11
#define LINUX_ENOTTY 25

// ---- Memory ----

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20
#define MAP_NORESERVE  0x4000

static inline void *
linux_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
  long r = linux_syscall(SYS_mmap, addr, len, prot, flags, fd, off);
  if(r < 0 && r > -4096)
    return NULL;
  return (void *)r;
}

// ---- Time ----

#define CLOCK_MONOTONIC 1
#define TIMER_ABSTIME   1

struct linux_timespec {
  long tv_sec;
  long tv_nsec;
};

struct linux_itimerspec {
  struct linux_timespec it_interval;
  struct linux_timespec it_value;
};

static inline uint64_t
linux_clock_gettime_ns(int clk)
{
  struct linux_timespec ts;
  linux_syscall(SYS_clock_gettime, clk, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// ---- Signals ----

#define SIGINT    2
#define SIGILL    4
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGSEGV  11
#define SIGTERM  15
#define SIGALRM  14
#define SIGIO    29
#define SIGHUP    1
#define SIGRTMIN 32

#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_NODEFER   0x40000000

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

#define SI_TIMER    -2
#define POLL_IN      1
#define POLL_HUP     6

#define SIGEV_SIGNAL     0
#define SIGEV_THREAD_ID  4

typedef unsigned long linux_sigset_t;

struct linux_sigaction {
  void *sa_handler;
  unsigned long sa_flags;
  void (*sa_restorer)(void);
  linux_sigset_t sa_mask;
};

typedef struct linux_siginfo {
  int si_signo;
  int si_errno;
  int si_code;
  int _pad0;
  union {
    struct {
      int si_tid;
      int si_overrun;
      long si_value;
    } timer;
    struct {
      long si_band;
      int si_fd;
    } poll;
    struct {
      void *si_addr;
    } fault;
    long _pad[14];
  };
} linux_siginfo_t;

_Static_assert(sizeof(linux_siginfo_t) == 128, "siginfo size");

struct linux_sigevent {
  long sigev_value;
  int sigev_signo;
  int sigev_notify;
  int sigev_tid;
  int _pad[11];
};

_Static_assert(sizeof(struct linux_sigevent) == 64, "sigevent size");

struct linux_stack {
  void *ss_sp;
  int ss_flags;
  size_t ss_size;
};

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

void __restore_rt(void); // entry.S

#define LINUX_SIGMASK(sig) (1ul << ((sig) - 1))

static inline int
linux_sigaction(int sig, void *handler, unsigned long flags,
                linux_sigset_t mask)
{
  struct linux_sigaction sa = {
    .sa_handler = handler,
    .sa_flags = flags | SA_RESTORER,
    .sa_restorer = __restore_rt,
    .sa_mask = mask,
  };
  return linux_syscall(SYS_rt_sigaction, sig, &sa, NULL, sizeof(linux_sigset_t));
}

static inline void
linux_sigsuspend_all(void)
{
  linux_sigset_t none = 0;
  linux_syscall(SYS_rt_sigsuspend, &none, sizeof(none));
}

// ---- Files / terminal ----

#define O_NONBLOCK  00004000
#define O_ASYNC     00020000

#define F_GETFL      3
#define F_SETFL      4
#define F_SETOWN     8
#define F_SETSIG    10

#define TCGETS 0x5401
#define TCSETS 0x5402

// c_iflag
#define ICRNL  0000400
#define IXON   0002000
// c_lflag
#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define IEXTEN 0100000

#define VTIME 5
#define VMIN  6

struct linux_termios {
  unsigned int c_iflag;
  unsigned int c_oflag;
  unsigned int c_cflag;
  unsigned int c_lflag;
  unsigned char c_line;
  unsigned char c_cc[19];
};

struct linux_pollfd {
  int fd;
  short events;
  short revents;
};

#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLHUP 0x0010

static inline int
linux_poll1(int fd, short events, const struct linux_timespec *timeout)
{
  struct linux_pollfd pfd = { .fd = fd, .events = events };
  int r = linux_syscall(SYS_ppoll, &pfd, 1, timeout, NULL, sizeof(linux_sigset_t));
  if(r <= 0)
    return r;
  return pfd.revents;
}

static inline void __attribute__((noreturn))
linux_exit_group(int code)
{
  while(1)
    linux_syscall(SYS_exit_group, code);
}

// ---- Sockets / processes (network backends) ----

#define SYS_writev       20
#define SYS_dup2         33
#define SYS_socket       41
#define SYS_connect      42
#define SYS_socketpair   53
#define SYS_fork         57

#define AF_UNIX       1
#define SOCK_STREAM   1
#define SOCK_CLOEXEC  02000000

struct linux_sockaddr_un {
  unsigned short sun_family;
  char sun_path[108];
};
