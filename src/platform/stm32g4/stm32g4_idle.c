#include "irq.h"

void  __attribute__((noreturn))
cpu_idle(void)
{
  while(1) {
    asm("wfi");
  }
}
