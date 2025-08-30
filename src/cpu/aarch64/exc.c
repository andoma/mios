#include <mios/mios.h>

#include <stdint.h>

#if 0
#endif


static void
trap(const char *what)
{
  panic("TRAP %s", what);
}

void
curr_el_sp0_sync(void)
{
  void *elr;
  asm volatile ("mrs %0, elr_el1\n\t" : "=r" (elr));
  uint32_t spsr_el1;
  asm volatile ("mrs %0, spsr_el1\n\t" : "=r" (spsr_el1));
  uint32_t esr;
  asm volatile ("mrs %0, esr_el1\n\t" : "=r" (esr));
  void *far;
  asm volatile ("mrs %0, far_el1\n\t" : "=r" (far));

  uint32_t inst = *(uint32_t *)elr;
  panic("Synchronous Exception @ SP0. ELR:%p (*ELR:0x%x) ESR:0x%08x FAR:%p SPSR:%x",
        elr, inst, esr, far, spsr_el1);
}

void
curr_el_sp0_fiq(void)
{
  trap(__FUNCTION__);
}

void
curr_el_sp0_serror(void)
{
  trap(__FUNCTION__);
}

void
curr_el_spx_sync(void)
{
  void *elr;
  asm volatile ("mrs %0, elr_el1\n\t" : "=r" (elr));
  uint32_t spsr_el1;
  asm volatile ("mrs %0, spsr_el1\n\t" : "=r" (spsr_el1));
  uint32_t esr;
  asm volatile ("mrs %0, esr_el1\n\t" : "=r" (esr));
  void *far;
  asm volatile ("mrs %0, far_el1\n\t" : "=r" (far));
  panic("Synchronous Exception @ SPX. ELR:%p ESR:0x%08x FAR:%p SPSR:%x",
        elr, esr, far, spsr_el1);
}

void
curr_el_spx_irq(void)
{
  trap(__FUNCTION__);
}

void
curr_el_spx_fiq(void)
{
  trap(__FUNCTION__);
}

void
curr_el_spx_serror(void)
{
  trap(__FUNCTION__);
}

void
curr_el_aarch64_sync(void)
{
  trap(__FUNCTION__);
}

void
curr_el_aarch64_irq(void)
{
  trap(__FUNCTION__);
}

void
curr_el_aarch64_fiq(void)
{
  trap(__FUNCTION__);
}

void
lower_el_aarch64_serror(void)
{
  trap(__FUNCTION__);
}

void
lower_el_aarch32_sync(void)
{
  trap(__FUNCTION__);
}

void
lower_el_aarch32_irq(void)
{
  trap(__FUNCTION__);
}

void
lower_el_aarch32_fiq(void)
{
  trap(__FUNCTION__);
}

void
lower_el_aarch32_serror(void)
{
  trap(__FUNCTION__);
}

