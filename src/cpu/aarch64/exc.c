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
  uint32_t esr;
  asm volatile ("mrs %0, esr_el1\n\t" : "=r" (esr));
  void *far;
  asm volatile ("mrs %0, far_el1\n\t" : "=r" (far));
  panic("Synchronous Exception @ SP0. ELR:%p ESR:0x%08x FAR:%p", elr, esr, far);
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
  trap(__FUNCTION__);
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

