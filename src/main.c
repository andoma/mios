#include "mios.h"


volatile int x = 0;

int
main()
{
  static volatile unsigned int * const SYST_CSR = (unsigned int *)0xe000e010;
  static volatile unsigned int * const SYST_RVR = (unsigned int *)0xe000e014;

  *SYST_RVR = 0xfffff;
  *SYST_CSR = 7;
  while(1) {

  }
}
