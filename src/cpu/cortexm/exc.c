#include <stdio.h>
#include <stdint.h>

#include <mios/task.h>
#include <mios/mios.h>

#include "cpu.h"
#include "mpu.h"

//static volatile unsigned int * const UART0DR = (unsigned int *)0x4000c000;
//static volatile unsigned int * const CPUID   = (unsigned int *)0xe000ed00;
//static volatile unsigned int * const AIRCR   = (unsigned int *)0xe000ed0c;
//static volatile unsigned int * const ICSR    = (unsigned int *)0xe000ed04;
static volatile unsigned int * const HFSR    = (unsigned int *)0xe000ed2c;
static volatile unsigned int * const CFSR    = (unsigned int *)0xe000ed28;
//static volatile unsigned int * const MMAR    = (unsigned int *)0xe000ed34;

static volatile unsigned short * const UFSR    = (unsigned short *)0xe000ed2a;
static volatile unsigned char * const MMFSR    = (unsigned char *)0xe000ed28;
static volatile unsigned char * const BFSR    = (unsigned char *)0xe000ed29;
static volatile unsigned int * const MMFAR    = (unsigned int *)0xe000ed34;
static volatile unsigned int * const BFAR    = (unsigned int *)0xe000ed38;

__attribute__((noreturn))
void
exc_nmi(void *frame)
{
  panic_frame(frame, "NMI");
}

__attribute__((noreturn))
void
exc_hard_fault(void *frame)
{
  mpu_disable();

  panic_frame(frame, "Hard fault: HFSR:0x%08x CFSR:0x%08x\n",
              *HFSR, *CFSR);
}


__attribute__((noreturn))
void
exc_mm_fault(void *frame)
{
  mpu_disable();

  uint32_t addr = *MMFAR;
#ifdef CPU_STACK_REDZONE_SIZE
  thread_t *const t = thread_current();
  if(t && ((addr & ~(CPU_STACK_REDZONE_SIZE - 1)) == (intptr_t)t->t_sp_bottom)) {
    panic_frame(frame,
                "REDZONE HIT task:\"%s\" MFSR:0x%08x address:0x%08x",
                t->t_name, *MMFSR, addr);
  }
#endif
  panic_frame(frame, "MM fault at address:0x%08x MMFSR:0x%02x", addr, *MMFSR);
}

__attribute__((noreturn))
void
exc_bus_fault(void *frame)
{
  mpu_disable();
  panic_frame(frame, "Bus fault: 0x%08x at 0x%08x", *BFSR, *BFAR);
}



int
exc_handle_usage_fault(void)
{
#ifdef __ARM_FP
  uint16_t ufsr = *UFSR;
  if(ufsr == 0x8) {
    // NOCP (ie, tried to use FPU)

    thread_t *const t = thread_current();
    if(t == NULL || t->t_fpuctx == NULL) {
      return -1;
    }

    cpu_t *cpu = curcpu();

    cpu_fpu_enable(1);
    if(cpu->sched.current_fpu) {
      int32_t *ctx = cpu->sched.current_fpu->t_fpuctx;
      asm volatile("vstm %0, {s0-s15}" :: "r"(ctx));
      asm volatile("vstm %0, {s16-s31}" :: "r"(ctx + 16));
      uint32_t fpscr;
      asm volatile("vmrs %0, fpscr" :"=r"(fpscr));
      ctx[32] = fpscr;
    }

    cpu->sched.current_fpu = t;
    const int32_t *ctx = t->t_fpuctx;
    asm volatile("vldm %0, {s0-s15}" :: "r"(ctx));
    asm volatile("vldm %0, {s16-s31}" :: "r"(ctx + 16));
    asm volatile("vmsr fpscr, %0" :: "r"(ctx[32]));
    return 0;
  }
#endif
  return -1;
}


__attribute__((noreturn))
void
exc_usage_fault(void *frame)
{
  uint16_t ufsr = *UFSR;

  if(ufsr & 0x2) {
    // Most likely an attempt to return to non-thumb code, etc
    panic_frame(frame, "Invalid use of EPSR");
  }
  if(ufsr & 0x100) {
    panic_frame(frame, "Unaligned access");
  }
  panic_frame(frame, "Usage fault: 0x%x", ufsr);
}

__attribute__((noreturn))
void
exc_reserved(void)
{
  panic("Res");
}


__attribute__((noreturn))
void
exc_svc(void)
{
  panic("SVC");
}
