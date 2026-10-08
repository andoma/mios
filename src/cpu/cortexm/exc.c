#include <stdio.h>
#include <stdint.h>

#include <mios/task.h>
#include <mios/mios.h>
#include <mios/cli.h>

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



#ifdef __ARM_FP
static uint32_t fpu_nocp_restores;

static error_t
cmd_fpustats(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "Lazy FPU restores: NOCP=%u", (unsigned)fpu_nocp_restores);
#ifdef CPU_FPU_ICI_RESUME
  cli_printf(cli, " ICI=%u", (unsigned)cpu_fpu_ici_restores);
#endif
  cli_printf(cli, "\n");
  return 0;
}

CLI_CMD_DEF_EXT("fpustats", cmd_fpustats, "", "Lazy FPU acquisition counters");
#endif


int
exc_handle_usage_fault(void)
{
#ifdef HAVE_PSPLIM
  if(*UFSR & 0x10) {
    // STKOF - Stack overflow via PSPLIM
    return -1; // Escalate to exc_usage_fault
  }
#endif
#ifdef __ARM_FP
  uint16_t ufsr = *UFSR;
  thread_t *const t = thread_current();
  if(t == NULL || t->t_fpuctx == NULL)
    return -1;

  if(ufsr == 0x8) {
    cpu_fpu_switch(t);
    // NOCP is sticky and write-one-to-clear. Leave all other faults fatal.
    *UFSR = 0x8;
    fpu_nocp_restores++;
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

#ifdef HAVE_PSPLIM
  if(ufsr & 0x10) {
    thread_t *const t = thread_current();
    panic_frame(frame, "Stack overflow task:\"%s\"",
                t ? t->t_name : "?");
  }
#endif
  if(ufsr & 0x2) {
    // Most likely an attempt to return to non-thumb code, etc
#ifdef HAVE_FPU
    const uint32_t cpacr = *(volatile uint32_t *)0xe000ed88;
    // Capture before panic teardown. Pointer identities avoid dereferencing
    // a potentially corrupt owner and distinguish disabled-FPU resume from
    // an INVSTATE after the lazy restore has already completed.
    panic_frame(frame, "Invalid use of EPSR (UFSR:%04x CPACR:%08x "
                "thread:%p FP-owner:%p)", ufsr, (unsigned)cpacr,
                thread_current(), curcpu()->sched.current_fpu);
#else
    panic_frame(frame, "Invalid use of EPSR (UFSR:0x%04x)", ufsr);
#endif
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
