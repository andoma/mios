#pragma once

/*
 * Raw Linux syscall interface.
 *
 * Mios is built with -nostdinc and its own libc, so nothing from glibc
 * is available (or wanted: the symbol names collide). These are the
 * only syscalls the host port uses. Structures follow the kernel ABI
 * (uapi), not glibc's.
 *
 * Everything the two supported machines disagree on -- syscall numbers,
 * the syscall instruction, the signal frame -- lives in
 * linux_${arch}.h. The rest of the kernel ABI we touch (signal numbers,
 * fcntl and termios constants, siginfo, ...) comes from
 * include/uapi/asm-generic and is the same on both.
 */

#include <stdint.h>
#include <stddef.h>

typedef unsigned long linux_sigset_t;

// sigaltstack(2). Defined up here because struct linux_ucontext in the
// arch header embeds it.
struct linux_stack {
  void *ss_sp;
  int ss_flags;
  size_t ss_size;
};

#if defined(__x86_64__)
#include "linux_x86_64.h"
#elif defined(__aarch64__)
#include "linux_aarch64.h"
#else
#error Unsupported host architecture
#endif

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
#define SIGCHLD  17
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

void __restore_rt(void); // entry_${arch}.S

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

#define AT_FDCWD    -100

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

// The generic syscall table dropped open(2) in favour of openat(2)
static inline long
linux_open(const char *path, int flags, int mode)
{
#ifdef SYS_open
  return linux_syscall(SYS_open, path, flags, mode);
#else
  return linux_syscall(SYS_openat, AT_FDCWD, path, flags, mode);
#endif
}

static inline void __attribute__((noreturn))
linux_exit_group(int code)
{
  while(1)
    linux_syscall(SYS_exit_group, code);
}

// ---- Sockets / processes (network backends) ----

#define AF_UNIX       1
#define SOCK_STREAM   1
#define SOCK_CLOEXEC  02000000

struct linux_sockaddr_un {
  unsigned short sun_family;
  char sun_path[108];
};

// ... and dup2(2) in favour of dup3(2), which unlike dup2() rejects
// oldfd == newfd. No caller needs that case.
static inline long
linux_dup2(int oldfd, int newfd)
{
#ifdef SYS_dup2
  return linux_syscall(SYS_dup2, oldfd, newfd);
#else
  return linux_syscall(SYS_dup3, oldfd, newfd, 0);
#endif
}

// ... and fork(2) in favour of plain clone(2)
static inline long
linux_fork(void)
{
#ifdef SYS_fork
  return linux_syscall(SYS_fork);
#else
  return linux_syscall(SYS_clone, SIGCHLD, 0);
#endif
}

// ---- Threads / futex (virtual time simulation threads, sim.c) ----

#define CLONE_VM       0x00000100
#define CLONE_FS       0x00000200
#define CLONE_FILES    0x00000400
#define CLONE_SIGHAND  0x00000800
#define CLONE_THREAD   0x00010000
#define CLONE_SYSVSEM  0x00040000

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

// entry_${arch}.S: clone a thread on the given stack and run fn(arg) on it
long linux_clone_thread(unsigned long flags, void *child_sp,
                        void (*fn)(void *arg), void *arg);

static inline void
linux_futex_wait(volatile uint32_t *addr, uint32_t expected)
{
  linux_syscall(SYS_futex, addr, FUTEX_WAIT, expected, NULL, NULL, 0);
}

static inline void
linux_futex_wake(volatile uint32_t *addr)
{
  linux_syscall(SYS_futex, addr, FUTEX_WAKE, 1, NULL, NULL, 0);
}
