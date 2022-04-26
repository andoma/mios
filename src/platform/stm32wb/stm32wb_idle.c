#include "irq.h"

#include <mios/device.h>

#include "stm32wb_reg.h"
#include "stm32wb_clk.h"
#include "stm32wb_pwr.h"


static volatile unsigned int * const SCR  = (unsigned int *)0xe000ed10;

static int wakelock = 0x10000000;


void
wakelock_acquire(void)
{
  int q = irq_forbid(IRQ_LEVEL_ALL);
  wakelock++;
  irq_permit(q);
}

void
wakelock_release(void)
{
  int q = irq_forbid(IRQ_LEVEL_ALL);
  wakelock--;
  irq_permit(q);
}

void  __attribute__((noreturn))
cpu_idle(void)
{
  while(1) {
    if(!wakelock) {
      asm volatile ("cpsid i;isb");
      *SCR = 0x4;
      device_power_state(DEVICE_POWER_STATE_SUSPEND);
      asm("wfi");
      *SCR = 0x0;
      stm32wb_use_hse();
      device_power_state(DEVICE_POWER_STATE_RESUME);
      asm volatile ("cpsie i;isb");
    } else {
      asm("wfi");
    }
  }
}


void
suspend_enable(void)
{
  int q = irq_forbid(IRQ_LEVEL_ALL);
  if(wakelock & 0x10000000) {
    int mode = 1;
    reg_set_bits(PWR_CR1, 0, 3, mode);
    reg_set_bits(PWR_C2R1, 0, 3, mode);
    wakelock &= ~0x10000000;
  }
  irq_permit(q);
}
