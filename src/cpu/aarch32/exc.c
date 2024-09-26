#include <stdio.h>
#include <stdint.h>

#include <mios/mios.h>

#include "cpu.h"

void
data_abort(uint32_t pc, uint32_t psr, uint32_t *regs)
{
  printf("\n*** Exception: Data Abort\n");
  for(int i = 0; i < 13; i++) {
    printf("R%-2d 0x%08x\n", i, regs[i]);
  }
  printf("PC  0x%08x\nPSR 0x%08x\n", pc, psr);

  panic("Data abort");
}
