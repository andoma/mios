#pragma once

#ifdef __ARM_FP
#define HAVE_FPU
#define FPU_CTX_SIZE (33 * 4) // s0 ... s31 + FPSCR
#endif
