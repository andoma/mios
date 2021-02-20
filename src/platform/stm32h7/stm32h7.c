#include <stdio.h>
#include <malloc.h>



static void  __attribute__((constructor(120)))
stm32h7_init(void)
{
  extern unsigned long _ebss;

  void *DTCM_start = (void *)&_ebss;
  void *DTCM_end   = (void *)0x20000000 + 128 * 1024;

  printf("\nSTM32H7\n");

  // DTCM
  heap_add_mem((long)DTCM_start, (long)DTCM_end, 0);

}
