#include <stdio.h>
#include <stdint.h>

#include "task.h"
#include "cpu.h"
#include "mios.h"

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


void
exc_nmi(void)
{
  panic("NMI");
}

void
exc_hard_fault(void)
{
  panic("HARD FAULT, HFSR:0x%x CFSR:0x%x\n",
        *HFSR, *CFSR);
}

void
exc_mm_fault(void)
{
  uint32_t *psp;
  asm volatile ("mrs %0, psp\n\t" : "=r" (psp));

  panic("MM fault: 0x%x address:0x%x by 0x%x", *MMFSR, *MMFAR, psp[6]);
}

void
exc_bus_fault(void)
{
  uint32_t *psp;
  asm volatile ("mrs %0, psp\n\t" : "=r" (psp));

  panic("Bus fault: 0x%x at 0x%x by 0x%x", *BFSR, *BFAR, psp[6]);
}



void
exc_usage_fault(void)
{
  uint16_t ufsr = *UFSR;
#ifdef __ARM_FP
  if(ufsr & 0x8) {
    // NOCP (ie, tried to use FPU)

    task_t *const t = task_current();

    if(t == NULL || t->t_fpuctx == NULL) {
      panic("Task %s tries to use FPU but is not allowed",
            t ? t->t_name : "<none>");
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
    return;
  }
#endif
  panic("Usage fault: 0x%x sp=%p", ufsr, __builtin_frame_address(0));
}

void
exc_reserved(void)
{
  panic("Res");
}


void
exc_svc(void)
{
  panic("SVC");
}
