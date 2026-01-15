#include <stdio.h>
#include <malloc.h>

#include "drivers/pl011.h"

// Semihosting definitions
#define SEMIHOSTING_SYS_EXIT            0x18
#define ADP_STOPPED_APPLICATION_EXIT 0x20026
extern uintptr_t semihosting_call(uint32_t reason, uintptr_t arg);

long
gicr_base(void)
{
  return GIC_GICR_BASE;
}


static void __attribute__((constructor(101)))
board_init_early(void)
{
  heap_add_mem(HEAP_START_EBSS, 0xffff000000200000ull + 2 * 1024 * 1024,
               MEM_TYPE_DMA, 10);
  stdio = pl011_uart_init(0x09000000, 115200, 33);

  long wut;
  asm volatile("mrs %0, ID_AA64MMFR0_EL1\n\r" : "=r"(wut));
  printf("ID_AA64MMFR0_EL1 = %lx\n", wut);
}

void 
semihosting_exit(uint32_t reason, uint32_t subcode)
{
  uint64_t parameters[] = {reason, subcode};

  (void)semihosting_call(SEMIHOSTING_SYS_EXIT, (uintptr_t)&parameters);
}

void
reboot(void)
{
  semihosting_exit(ADP_STOPPED_APPLICATION_EXIT, 0);
  // should not reach here

  printf("Reboot not implemented, stalling forever\n");

  while(1) {
    asm volatile("wfi");
  }
}
