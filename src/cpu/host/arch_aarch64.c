/*
 * AArch64 specifics of the host CPU layer: building context switch
 * frames by hand, and decoding a signal frame for panic output.
 * See entry_aarch64.S for the frame layout.
 */

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include <stdio.h>

#include <mios/stream.h>

#include "cpu.h"
#include "linux.h"

// 20 registers, see entry_aarch64.S
#define FRAME_WORDS 20

#define FRAME_X19 0
#define FRAME_X20 1
#define FRAME_X21 2
#define FRAME_X30 11

void *
cpu_stack_init(uint64_t *stack, void *entry,
               void (*thread_exit)(void *), int nargs, va_list ap)
{
  uint64_t *p = (uint64_t *)((uintptr_t)stack & ~15) - FRAME_WORDS;
  memset(p, 0, FRAME_WORDS * sizeof(uint64_t));

  p[FRAME_X30] = (uint64_t)cpu_thread_start;
  p[FRAME_X19] = (uint64_t)entry;
  p[FRAME_X20] = (uint64_t)thread_exit;
  for(int i = 0; i < nargs && i < 4; i++)
    p[FRAME_X21 + i] = va_arg(ap, uintptr_t);
  return p;
}


void *
cpu_coswitch_frame_init(void *stack_top, void (*fn)(void))
{
  uint64_t *p = (uint64_t *)((uintptr_t)stack_top & ~15) - FRAME_WORDS;
  memset(p, 0, FRAME_WORDS * sizeof(uint64_t));
  p[FRAME_X30] = (uint64_t)fn;   // return address for the first coswitch
  return p;
}


void
host_regs_print(struct stream *st, const struct linux_ucontext *uc)
{
  const struct linux_sigcontext *mc = &uc->uc_mcontext;
  stprintf(st, "  pc  0x%016lx  sp  0x%016lx  pstate 0x%lx\n",
           mc->pc, mc->sp, mc->pstate);
  for(int i = 0; i < 30; i += 4) {
    stprintf(st, " ");
    for(int j = i; j < i + 4 && j < 31; j++)
      stprintf(st, " x%-2d 0x%016lx", j, mc->regs[j]);
    stprintf(st, "\n");
  }
}


// The kernel puts a {fp, lr} frame record with lr = the sigreturn
// trampoline on top of every signal frame, so the AAPCS64 frame chain
// walks straight through them. The ucontext itself sits below the
// record at an offset that varies with the FP/SVE state saved, so just
// report where the frames are.
void
host_sigframes_print(struct stream *st, const uintptr_t *sp,
                     const uintptr_t *top)
{
  for(; sp < top; sp++) {
    if(*sp != (uintptr_t)__restore_rt)
      continue;
    stprintf(st, "  sigframe @%p: fp=%lx\n", sp - 1, sp[-1]);
  }
}
