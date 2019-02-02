#include <stdio.h>
#include <assert.h>

#include "task.h"
#include "irq.h"
#include "mios.h"



static void *
main3(void *aux)
{
  int d = 1000;
  while(1) {
    printf("hello in main3: %d\n", d);
    d++;
    sleephz(20);
  }
  return NULL;
}



static void *
main2(void *aux)
{
  int d = 0;

  if(0) {
    task_create(main3, NULL, 256, "main3");
  }


  while(1) {
    printf("hello in main2: %d\n", d);
    d++;
    sleephz(HZ);
  }
  return NULL;
}



int
main(void)
{
  printf("Hello in main\n");
  return 0;

  while(1) {

  }

  if(1) {
    task_create(main2, NULL, 256, "main2");
  }
  printf("OK main exits\n");
}


