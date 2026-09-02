/*
 * Interrupt controller and exception entry for the host CPU.
 *
 * Model:
 *
 *  - Peripherals raise IRQs by signalling the process: a POSIX timer
 *    (timer.c, SIGALRM) and O_ASYNC file descriptors (console, network
 *    backends, SIGIO). Standard signals coalesce, so a signal means
 *    "something on this class of lines", and every line of that class
 *    is polled. See HOST_SIG_* in irq.h for why not realtime signals.
 *
 *  - host_signal() is exception entry. It marks the line pending and
 *    calls irq_dispatch(), which runs every pending handler the current
 *    irq_level allows, highest priority first. A handler runs with
 *    irq_level set to its own level, so a nested signal at a lower or
 *    equal priority only sets a pending bit (like NVIC preemption).
 *
 *  - schedule() sets the SWITCH bit (level 7, lowest). When dispatch
 *    reaches it, cpu_switch() saves callee-saved registers and calls the
 *    kernel's task_switch(). This may happen inside the signal handler:
 *    the interrupted thread's kernel sigframe simply stays on its stack
 *    until we switch back and the handler returns through it.
 *
 *  - Signal masking. The IRQ signals are blocked by the kernel when a
 *    handler is entered (sa_mask, no SA_NODEFER) and irq_dispatch()
 *    unblocks them again right before it runs a handler or switches
 *    thread. This is deliberate: with SA_NODEFER, a peer writing to a
 *    socket in a burst re-pends SIGIO after every frame the kernel sets
 *    up, and the kernel keeps stacking frames (3.3kB each) without ever
 *    returning to user space. With this scheme a nested handler whose
 *    level is masked just records the line and returns, never unblocks,
 *    so nesting depth is bounded by the number of IRQ levels, like on
 *    a real NVIC. Costs one rt_sigprocmask per dispatched interrupt.
 *
 *  - irq_level and irq_depth act as CPU registers (basepri / IPSR).
 *    Every path that changes them restores them from locals before
 *    returning, so after any preemption they read what the interrupted
 *    code expects.
 */

#include <string.h>
#include <stdio.h>

#include <mios/stream.h>
#include <mios/unwind.h>
#include <mios/task.h>

#include "irq.h"
#include "cpu.h"
#include "linux.h"

// Boot runs "in handler mode" with everything masked, like a Cortex-M
// before the first cpsie. These are in .data (non-zero initializers)
// so init()'s bss clear leaves them alone.
volatile unsigned int irq_level = IRQ_LEVEL_ALL;
volatile int irq_depth = 1;

volatile unsigned int irq_primask;
volatile uint32_t irq_pending;

typedef struct host_irq {
  void (*fn)(void *arg);
  void *arg;
  int fd;
  uint8_t level;
  uint8_t used;
  uint8_t timer;
} host_irq_t;

static host_irq_t host_irqs[HOST_IRQ_COUNT];

// Set on signal handler entry (kernel has just blocked the IRQ signals),
// cleared when irq_dispatch() unblocks them. Conservative: may read 1
// when signals are in fact unblocked, never the other way around.
static volatile int irq_sigs_blocked;

static const linux_sigset_t irq_sigmask =
  LINUX_SIGMASK(HOST_SIG_TIMER) | LINUX_SIGMASK(HOST_SIG_IO);

static void
irq_sigs_unblock(void)
{
  if(irq_sigs_blocked) {
    irq_sigs_blocked = 0;
    linux_syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &irq_sigmask, NULL,
                  sizeof(irq_sigmask));
  }
}


void
irq_dispatch(void)
{
  const int depth = irq_depth;
  irq_depth = depth + 1;
  irq_barrier();

  while(!irq_primask) {
    const unsigned int cur = irq_level;
    uint32_t p = __atomic_load_n(&irq_pending, __ATOMIC_SEQ_CST);
    int best = -1;
    unsigned int best_level = 0xff;

    while(p) {
      const int i = __builtin_ctz(p);
      p &= p - 1;
      const unsigned int lvl =
        i == HOST_IRQ_SWITCH ? IRQ_LEVEL_SWITCH : host_irqs[i].level;
      if(cur && lvl >= cur)
        continue;
      if(lvl < best_level) {
        best_level = lvl;
        best = i;
      }
    }

    if(best < 0)
      break;

    // Raise level first, then claim the bit. A nested handler landing
    // in between either sees the line blocked or finds the bit gone.
    irq_level = best_level;
    irq_barrier();
    const uint32_t bit = 1u << best;
    const uint32_t prev = __atomic_fetch_and(&irq_pending, ~bit, __ATOMIC_SEQ_CST);
    if(prev & bit) {
      irq_sigs_unblock();
      if(best == HOST_IRQ_SWITCH) {
        cpu_switch();
      } else if(host_irqs[best].fn) {
        host_irqs[best].fn(host_irqs[best].arg);
      }
    }
    // We may have been suspended and resumed in between, restore our view
    irq_level = cur;
    irq_depth = depth + 1;
    irq_barrier();
  }
  irq_depth = depth;
  irq_barrier();
}


static uint32_t
irq_bits_class(int timer)
{
  uint32_t bits = 0;
  for(int i = 0; i < HOST_IRQ_COUNT; i++) {
    if(host_irqs[i].used && (timer ? host_irqs[i].timer : host_irqs[i].fd >= 0))
      bits |= 1u << i;
  }
  return bits;
}


static void
host_signal(int sig, linux_siginfo_t *si, void *ucontext)
{
  irq_sigs_blocked = 1;
  const uint32_t bits = irq_bits_class(sig == HOST_SIG_TIMER);
  __atomic_fetch_or(&irq_pending, bits, __ATOMIC_SEQ_CST);
  irq_dispatch();
}


static const char *
signame(int sig)
{
  switch(sig) {
  case SIGSEGV: return "SIGSEGV";
  case SIGBUS:  return "SIGBUS";
  case SIGILL:  return "SIGILL";
  case SIGFPE:  return "SIGFPE";
  case SIGABRT: return "SIGABRT";
  default:      return "signal";
  }
}


static void
host_fatal(int sig, linux_siginfo_t *si, void *ucontext)
{
  struct linux_ucontext *uc = ucontext;
  thread_t *t = thread_current();
  panic_frame(uc, "%s at %p (rip=0x%lx) thread:%s stack:%p-%p "
              "irq_level:%d irq_depth:%d pending:0x%x",
              signame(sig), si->fault.si_addr, uc->uc_mcontext.rip,
              t ? t->t_name : "?", t ? t->t_sp_bottom : NULL,
              t ? (void *)t : NULL, irq_level, irq_depth, irq_pending);
}


void
backtrace_print_frame(struct stream *st, void *frame)
{
  if(frame == NULL)
    return;
  const struct linux_sigcontext *mc = &((struct linux_ucontext *)frame)->uc_mcontext;
  stprintf(st, "  rip 0x%016lx  rsp 0x%016lx  rbp 0x%016lx\n",
           mc->rip, mc->rsp, mc->rbp);
  stprintf(st, "  rax 0x%016lx  rbx 0x%016lx  rcx 0x%016lx  rdx 0x%016lx\n",
           mc->rax, mc->rbx, mc->rcx, mc->rdx);
  stprintf(st, "  rsi 0x%016lx  rdi 0x%016lx  r8  0x%016lx  r9  0x%016lx\n",
           mc->rsi, mc->rdi, mc->r8, mc->r9);
  stprintf(st, "  r10 0x%016lx  r11 0x%016lx  r12 0x%016lx  r13 0x%016lx\n",
           mc->r10, mc->r11, mc->r12, mc->r13);
  stprintf(st, "  r14 0x%016lx  r15 0x%016lx  eflags 0x%lx\n",
           mc->r14, mc->r15, mc->eflags);
  stprintf(st, "  (addr2line -e build.host/mios.full.elf 0x%lx)\n", mc->rip);

  // Crude backtrace: every word on the stack that points into .text
  extern char _stext, _text_end;
  thread_t *t = thread_current();
  if(t == NULL)
    return;
  const uintptr_t *sp = (const uintptr_t *)(mc->rsp & ~7);
  const uintptr_t *top = (const uintptr_t *)t;
  int n = 0;
  stprintf(st, "  text words on stack (%zd bytes):", (size_t)((void *)top - (void *)sp));
  for(; sp < top && n < 64; sp++) {
    if(*sp >= (uintptr_t)&_stext && *sp < (uintptr_t)&_text_end) {
      stprintf(st, " %lx", *sp);
      n++;
    }
  }
  stprintf(st, "\n");

  // Signal frames on the stack: pretcode is __restore_rt, ucontext follows
  for(sp = (const uintptr_t *)(mc->rsp & ~7); sp < top; sp++) {
    if(*sp != (uintptr_t)__restore_rt)
      continue;
    const struct linux_ucontext *uc = (const void *)(sp + 1);
    const linux_siginfo_t *si = (const void *)(uc + 1);
    stprintf(st, "  sigframe @%p: rip=%lx rsp=%lx sig=%d code=%d mask=%lx\n", sp,
             uc->uc_mcontext.rip, uc->uc_mcontext.rsp,
             si->si_signo, si->si_code, uc->uc_sigmask);
  }
}


static void
host_terminate(int sig, linux_siginfo_t *si, void *ucontext)
{
  host_exit(128 + sig);
}


// Called from host_start() before init(). Only touches kernel state
// (init() clears .bss right after this).
void
host_irq_init(void)
{
  linux_sigaction(HOST_SIG_TIMER, host_signal, SA_SIGINFO, irq_sigmask);
  linux_sigaction(HOST_SIG_IO, host_signal, SA_SIGINFO, irq_sigmask);

  // Faults get their own stack so a blown thread stack still yields a
  // readable panic.
  const size_t altsize = 256 * 1024;
  void *alt = linux_mmap(NULL, altsize, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  struct linux_stack ss = { .ss_sp = alt, .ss_flags = 0, .ss_size = altsize };
  linux_syscall(SYS_sigaltstack, &ss, NULL);

  const int fatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
  for(size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
    linux_sigaction(fatal[i], host_fatal, SA_SIGINFO | SA_ONSTACK, 0);

  linux_sigaction(SIGTERM, host_terminate, SA_SIGINFO, 0);
  linux_sigaction(SIGHUP, host_terminate, SA_SIGINFO, 0);
  linux_sigaction(SIGINT, host_terminate, SA_SIGINFO, 0);

  linux_sigset_t none = 0;
  linux_syscall(SYS_rt_sigprocmask, SIG_SETMASK, &none, NULL, sizeof(none));
}


// ---- IRQ line management ----

int
host_irq_alloc(int level, void (*fn)(void *arg), void *arg)
{
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  int r = -1;
  for(int i = 0; i < HOST_IRQ_COUNT; i++) {
    if(host_irqs[i].used)
      continue;
    host_irqs[i].used = 1;
    host_irqs[i].fd = -1;
    host_irqs[i].level = level;
    host_irqs[i].fn = fn;
    host_irqs[i].arg = arg;
    r = i;
    break;
  }
  irq_permit(q);
  if(r < 0)
    panic("Out of host IRQ lines");
  return r;
}


void
host_irq_attach_fd(int irq, int fd)
{
  host_irqs[irq].fd = fd;

  const long pid = linux_syscall(SYS_getpid);
  // These settings live in the open file description, which for a
  // terminal is shared with the shell that started us (and anything it
  // starts later). Reset the signal explicitly: a stale F_SETSIG from
  // another program would deliver a signal we have no handler for.
  linux_syscall(SYS_fcntl, fd, F_SETSIG, 0);
  linux_syscall(SYS_fcntl, fd, F_SETOWN, pid);
  long fl = linux_syscall(SYS_fcntl, fd, F_GETFL);
  linux_syscall(SYS_fcntl, fd, F_SETFL, fl | O_ASYNC);
}


void
host_irq_detach_fd(int fd)
{
  long fl = linux_syscall(SYS_fcntl, fd, F_GETFL);
  linux_syscall(SYS_fcntl, fd, F_SETFL, fl & ~O_ASYNC);
}


void
host_irq_attach_timer(int irq)
{
  host_irqs[irq].timer = 1;
}


void
host_irq_raise(int irq)
{
  __atomic_fetch_or(&irq_pending, 1u << irq, __ATOMIC_SEQ_CST);
  irq_barrier();
  if(!irq_primask)
    irq_dispatch();
}


void
irq_enable_fn_arg(int irq, int level, void (*fn)(void *arg), void *arg)
{
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  host_irqs[irq].used = 1;
  host_irqs[irq].level = level;
  host_irqs[irq].fn = fn;
  host_irqs[irq].arg = arg;
  irq_permit(q);
}


void
irq_enable_fn_fpu(int irq, int level, void (*fn)(void *arg), void *arg)
{
  irq_enable_fn_arg(irq, level, fn, arg);
}


static void
irq_call_void(void *arg)
{
  void (*fn)(void) = arg;
  fn();
}


void
irq_enable_fn(int irq, int level, void (*fn)(void))
{
  irq_enable_fn_arg(irq, level, irq_call_void, fn);
}


void
irq_enable(int irq, int level)
{
  // Vector-table style irq_N() handlers have no meaning here
  panic("irq_enable(%d) without handler is not supported on host", irq);
}


void
irq_disable(int irq)
{
  const int q = irq_forbid(IRQ_LEVEL_ALL);
  host_irqs[irq].fn = NULL;
  irq_permit(q);
}
