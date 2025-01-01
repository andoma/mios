#include <stdio.h>

#include "reg.h"

static volatile unsigned int * const UART0DR = (unsigned int *) 0x09000000;

static void
printchar(char c)
{
  *UART0DR = c;
}


static ssize_t
console_write(stream_t *s, const void *buf, size_t size, int flags)
{
  const char *d = buf;
  for(size_t i = 0; i < size; i++) {
    printchar(d[i]);
  }
  return size;
}


static const stream_vtable_t console_vtable = {
  .write = console_write
};

static struct stream console = {
  .vtable = &console_vtable
};

static void __attribute__((constructor(101)))
board_init_console(void)
{
  stdio = &console;
  asm volatile("":::"memory");

  //#define GIC_BASE 0x08000000
#define GIC_BASE 0xf0000000

  printf("GICD_CTRL:%x\n", reg_rd(GIC_BASE + 0x0));
  printf("GICD_TYPER:%x\n", reg_rd(GIC_BASE + 0x4));
  printf("GICD_IIDR:%x\n", reg_rd(GIC_BASE + 0x8));

  unsigned long sp_el0 = 0x41234560;
  asm volatile ("msr sp_el0, %0\n\t" : : "r" (sp_el0));


  //  unsigned int old;
  //  asm volatile ("mrs %0, primask\n\t" : "=r" (old));

#if 0
  register long x0 __asm__ ("x0");
  __asm__ ("mrs x0, CurrentEL;" : : : "%x0");
  printf("EL = %d\n", (int)x0 >> 2);
#endif
}
