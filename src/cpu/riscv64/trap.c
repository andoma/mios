#include <stdint.h>
#include <stdio.h>

#include "mios.h"

#define CLINT_BASE  0x2000000 // Should probably be in platform.h
volatile static uint32_t* msip  =      (uint32_t*)(CLINT_BASE + 0x0);


#if 1
inline long
test(void)
{
  long old;
  asm volatile ("csrrs %0, mtvec, zero\n\t" : "=r" (old));
  //  asm volatile ("csrrs %0, misa, zero\n\t" : "=r" (old));
  return old;
}

inline long
get_mstatus(void)
{
  long r;
  asm volatile ("csrrs %0, mstatus, zero\n\t" : "=r" (r));
  return r;
}


#endif

static int
sw_interrupt(void)
{
  const int hart = 0;
  msip[hart] = 0;
  return 1;
}



void
schedule(void)
{
  const int hart = 0;
  msip[hart] = 1;
}



extern void timer_interrupt(void);

static int
handle_interrupt(int cause)
{
  switch(cause) {
  case 3:
    return sw_interrupt();
  case 7:
    timer_interrupt();
    return 0;
  default:
    panic("Got unexpected interrupt %d", cause);
  }
}



int
handle_trap(long cause)
{
  if(cause & (1ull << 63)) {
    // Interrupt
    return handle_interrupt(cause);
  }
  panic("trap: cause:%lx",cause); //  pc:%lx mtval:%lx\n", cause, epc, mtval);
}
