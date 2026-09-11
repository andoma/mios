/*
 * x86-64 specifics of the host CPU layer: building context switch
 * frames by hand, and decoding a signal frame for panic output.
 * See entry_x86_64.S for the frame layout.
 */

#include <stdarg.h>
#include <stdint.h>

#include <stdio.h>

#include <mios/stream.h>

#include "cpu.h"
#include "linux.h"

void *
cpu_stack_init(uint64_t *stack, void *entry,
               void (*thread_exit)(void *), int nargs, va_list ap)
{
  uint64_t args[4] = {};
  for(int i = 0; i < nargs && i < 4; i++)
    args[i] = va_arg(ap, uintptr_t);

  uint64_t *p = (uint64_t *)((uintptr_t)stack & ~15);
  *--p = (uint64_t)cpu_thread_start;  // return address
  *--p = (uint64_t)thread_exit;       // rbp
  *--p = (uint64_t)entry;             // rbx
  *--p = args[0];                     // r12
  *--p = args[1];                     // r13
  *--p = args[2];                     // r14
  *--p = args[3];                     // r15
  return p;
}


void *
cpu_coswitch_frame_init(void *stack_top, void (*fn)(void))
{
  // Land in fn() with the SysV entry contract rsp%16==8 (as if reached
  // by a call). After cpu_coswitch pops the 6 saved regs and rets, rsp
  // equals this base, so make the base %16==8.
  uint64_t *sp = (uint64_t *)(((uintptr_t)stack_top & ~(uintptr_t)15) - 8);
  *--sp = (uint64_t)fn;   // return address for the first coswitch
  *--sp = 0;              // rbp
  *--sp = 0;              // rbx
  *--sp = 0;              // r12
  *--sp = 0;              // r13
  *--sp = 0;              // r14
  *--sp = 0;              // r15
  return sp;
}


void
host_regs_print(struct stream *st, const struct linux_ucontext *uc)
{
  const struct linux_sigcontext *mc = &uc->uc_mcontext;
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
}


// The kernel's rt_sigframe starts with the address of the sigreturn
// trampoline, followed by the ucontext and the siginfo.
void
host_sigframes_print(struct stream *st, const uintptr_t *sp,
                     const uintptr_t *top)
{
  for(; sp < top; sp++) {
    if(*sp != (uintptr_t)__restore_rt)
      continue;
    const struct linux_ucontext *uc = (const void *)(sp + 1);
    const linux_siginfo_t *si = (const void *)(uc + 1);
    stprintf(st, "  sigframe @%p: rip=%lx rsp=%lx sig=%d code=%d mask=%lx\n", sp,
             uc->uc_mcontext.rip, uc->uc_mcontext.rsp,
             si->si_signo, si->si_code, uc->uc_sigmask);
  }
}
