#include <stdint.h>
#include "mios.h"

static volatile unsigned int * const UART0DR = (unsigned int *)0x4000c000;

static volatile unsigned int * const ICSR    = (unsigned int *)0xe000ed04;
static volatile unsigned int * const HFSR    = (unsigned int *)0xe000ed2c;


int main();



void __attribute__ ((noinline))
puts(const char *s)
{
  for(; *s; s++) {
    *UART0DR = *s;
  }
}

void __attribute__ ((noinline))
puts32(unsigned int v)
{
  puts("0x");
  for(int i = 28; i >= 0; i -= 4) {
    *UART0DR = "0123456789abcdef"[(v >> i) & 0xf];
  }
}


static void  __attribute__ ((noreturn))
halt(const char *msg)
{
  puts("HALT:");
  puts(msg);
  puts("\n");
  while(1) {}
}



void
init(void)
{

  extern unsigned long _stext;
  extern unsigned long _sbss;
  extern unsigned long _sdata;
  extern unsigned long _etext;
  extern unsigned long _ebss;
  extern unsigned long _edata;

  unsigned long *src, *dst;

  src = &_etext;
  dst = &_sdata;
  while(dst < &_edata)
    *(dst++) = *(src++);

  src = &_sbss;
  while(src < &_ebss)
    *(src++) = 0;

  main();
}

void
exc_nmi(void)
{
  halt("NMI");
}

void
exc_hard_fault(void)
{
  puts("HFSR: ");
  puts32(*HFSR);
  halt("HARD FAULT");
}

void
exc_mm_fault(void)
{
  halt("MM");
}

void
exc_bus_fault(void)
{
  halt("Bus");
}
void
exc_usage_fault(void)
{
  halt("Usage");
}

void
exc_reserved(void)
{
  halt("Res");
}


void
exc_pendsv(void)
{
  halt("PendSV");
}

void
exc_systick(void)
{
  puts("SYSTICK, ICSR:");
  puts32(*ICSR);
  puts("\n");
}

void
svc_handler(void)
{
  puts("SVC");
}
