#include "mios.h"

//static volatile unsigned int * const UART0DR = (unsigned int *)0x4000c000;
//static volatile unsigned int * const CPUID   = (unsigned int *)0xe000ed00;
//static volatile unsigned int * const AIRCR   = (unsigned int *)0xe000ed0c;
//static volatile unsigned int * const ICSR    = (unsigned int *)0xe000ed04;
static volatile unsigned int * const HFSR    = (unsigned int *)0xe000ed2c;
static volatile unsigned int * const CFSR    = (unsigned int *)0xe000ed28;
//static volatile unsigned int * const MMAR    = (unsigned int *)0xe000ed34;


void
exc_nmi(void)
{
  panic("NMI");
}

void
exc_hard_fault(void)
{
  panic("HARD FAULT, HFSR:0x%x CFSR:0x%x\n",
        *HFSR, *CFSR);
}

void
exc_mm_fault(void)
{
  panic("MM");
}

void
exc_bus_fault(void)
{
  panic("Bus");
}
void
exc_usage_fault(void)
{
  panic("Usage");
}

void
exc_reserved(void)
{
  panic("Res");
}


void
exc_svc(void)
{
  panic("SVC");
}
