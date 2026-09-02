#pragma once

// Included on the command line (-include) for every translation unit,
// mirrors cortexm.h. Single CPU, so curcpu() is a constant.

#if !defined(curcpu) && !defined(ASM)
struct cpu;
extern struct cpu cpu0;
static inline struct cpu *curcpu(void) { return &cpu0; }
#endif

#define CACHE_LINE_SIZE 64
