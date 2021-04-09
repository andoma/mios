#pragma once

#ifdef __ARM_FP
#define HAVE_FPU
#define FPU_CTX_SIZE (33 * 4) // s0 ... s31 + FPSCR
#endif

#if !defined(curcpu) && !defined(ASM)
struct cpu;
extern struct cpu cpu0;
static inline struct cpu *curcpu(void) { return &cpu0; }
#endif
