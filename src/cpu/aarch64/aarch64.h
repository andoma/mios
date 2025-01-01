#pragma once

#if !defined(ASM)
struct cpu;
extern struct cpu cpu0;
static inline struct cpu *curcpu(void) { return &cpu0; }
#endif
