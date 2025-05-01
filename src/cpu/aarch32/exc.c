#include <stdio.h>
#include <stdint.h>

#include <mios/mios.h>

#include "cpu.h"

int ignore_data_abort;
uint32_t data_aborts;

void
data_abort(uint32_t pc, uint32_t psr, uint32_t *regs)
{
  data_aborts++;

  if(ignore_data_abort)
    return;
  printf("\n*** Exception: Data Abort\n");
  for(int i = 0; i < 13; i++) {
    printf("R%-2d  0x%08x\n", i, regs[i]);
  }
  printf("PC   0x%08x\nPSR  0x%08x\n", pc, psr);

  uint32_t dfsr;
  asm volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(dfsr));
  printf("DFSR 0x%08x\n", dfsr);

  uint32_t dfar;
  asm volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(dfar));
  printf("DFAR 0x%08x\n", dfar);

  panic("Data abort");
}


void
prefetch_abort(uint32_t pc, uint32_t psr, uint32_t *regs)
{
  printf("\n*** Exception: Prefetch Abort\n");
  for(int i = 0; i < 13; i++) {
    printf("R%-2d 0x%08x\n", i, regs[i]);
  }
  printf("PC  0x%08x\nPSR 0x%08x\n", pc, psr);

  panic("Prefetch abort");
}
