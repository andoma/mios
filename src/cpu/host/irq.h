#pragma once

/*
 * Interrupt controller emulation.
 *
 * Same level scheme as cortexm: lower number = higher priority.
 * irq_forbid(l) blocks every IRQ at level >= l (basepri semantics).
 *
 * The mask is a plain global (irq_level, 0 = nothing blocked). Signals
 * delivered while their level is blocked are recorded in irq_pending
 * and run when the level is lowered again. This makes the hot path
 * (irq_forbid/irq_permit, hundreds of call sites) a couple of memory
 * operations, no syscalls.
 */

#define IRQ_LEVEL_ALL      1

#define IRQ_LEVEL_PROFILE  1
#define IRQ_LEVEL_HIGH     2
#define IRQ_LEVEL_SCHED    3
#define IRQ_LEVEL_CONSOLE  3
#define IRQ_LEVEL_IO       4
#define IRQ_LEVEL_NET      5
#define IRQ_LEVEL_CLOCK    6
#define IRQ_LEVEL_SWITCH   7

#define IRQ_LEVEL_NONE -1 // Place holder value to signal no IRQ

#include <stdint.h>
#include <mios/mios.h>

// Number of allocatable host IRQs. Bit 31 of irq_pending is the
// context switch request (PendSV equivalent).
#define HOST_IRQ_COUNT   31
#define HOST_IRQ_SWITCH  31

extern volatile unsigned int irq_level;    // 0: none blocked, else >= level
extern volatile unsigned int irq_primask;  // irq_off(): everything blocked
extern volatile int irq_depth;             // >0 while running ISRs
extern volatile uint32_t irq_pending;

void irq_dispatch(void);

static inline void  __attribute__((always_inline))
irq_barrier(void)
{
  asm volatile ("" ::: "memory");
}

static inline void  __attribute__((always_inline))
irq_off(void)
{
  irq_primask = 1;
  irq_barrier();
}

static inline void  __attribute__((always_inline))
irq_ensure0(unsigned int level, const char *file, int line)
{
  if(irq_depth == 0 || irq_primask)
    return;
  const unsigned int cur = irq_level;
  if(!cur || cur > level) {
    panic("Insuficient IRQ blocking at %s:%d level:%d irq_level:%d\n",
          file, line, level, cur);
  }
}

#define irq_ensure(l) irq_ensure0(l, __FILE__, __LINE__)

static inline unsigned int  __attribute__((always_inline))
irq_forbid(unsigned int level)
{
  const unsigned int old = irq_level;
  if(old == 0 || level < old)
    irq_level = level;
  irq_barrier();
  return old;
}

static inline void  __attribute__((always_inline))
irq_permit(unsigned int old)
{
  irq_barrier();
  irq_level = old;
  irq_barrier();
  if(__atomic_load_n(&irq_pending, __ATOMIC_SEQ_CST))
    irq_dispatch();
}

static inline unsigned int  __attribute__((always_inline))
irq_lower(void)
{
  const unsigned int old = irq_level;
  irq_barrier();
  irq_level = 0;
  irq_barrier();
  if(__atomic_load_n(&irq_pending, __ATOMIC_SEQ_CST))
    irq_dispatch();
  return old;
}

static inline int  __attribute__((always_inline))
can_sleep(void)
{
  return irq_depth == 0 && !irq_primask;
}

static inline void  __attribute__((always_inline))
schedule(void)
{
  __atomic_fetch_or(&irq_pending, 1u << HOST_IRQ_SWITCH, __ATOMIC_SEQ_CST);
  if(irq_level == 0 && !irq_primask)
    irq_dispatch();
}

void irq_enable(int irq, int level);

void irq_enable_fn(int irq, int level, void (*fn)(void));

void irq_enable_fn_arg(int irq, int level, void (*fn)(void *arg), void *arg);

void irq_enable_fn_fpu(int irq, int level, void (*fn)(void *arg), void *arg);

void irq_disable(int irq);

static inline void  __attribute__((always_inline))
irq_ack(int irq)
{
}

// ---- Host specific ----

// Allocate an IRQ line and attach a handler. Returns IRQ number or -1.
int host_irq_alloc(int level, void (*fn)(void *arg), void *arg);

// Route readiness events (SIGIO) on fd to the given IRQ line.
void host_irq_attach_fd(int irq, int fd);

// Undo host_irq_attach_fd() on the descriptor (leave shared terminals clean)
void host_irq_detach_fd(int fd);

// Mark the line as driven by a POSIX timer (SIGALRM)
void host_irq_attach_timer(int irq);

// Software-raise an IRQ line (dispatches immediately if unmasked)
void host_irq_raise(int irq);

// Mark an IRQ line pending without dispatching. The only way another
// Linux thread (a simulation peer) may signal Mios; the CPU thread
// dispatches when it resumes.
static inline void
host_irq_pend(int irq)
{
  __atomic_fetch_or(&irq_pending, 1u << irq, __ATOMIC_SEQ_CST);
}

// Signals used for IRQ delivery. Standard (non-queuing) signals; SIGIO
// cannot tell us which fd fired, so every fd line gets polled.
#define HOST_SIG_TIMER SIGALRM
#define HOST_SIG_IO    SIGIO
