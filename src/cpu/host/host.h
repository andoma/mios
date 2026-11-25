#pragma once

// Included on the command line (-include) for every translation unit,
// mirrors cortexm.h. Single CPU, so curcpu() is a constant.

#if !defined(curcpu) && !defined(ASM)
struct cpu;
extern struct cpu cpu0;
static inline struct cpu *curcpu(void) { return &cpu0; }
#endif

#define CACHE_LINE_SIZE 64

// The host port (platforms host and hostlib): a Linux process, so floating
// point is there for code that otherwise needs HAVE_FPU (which is about
// Cortex-M lazy FPU stacking and stays undefined here)
#define MIOS_HOST 1
