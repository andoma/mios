#include <stdio.h>
#include <assert.h>

#include "sys.h"
#include "task.h"
#include "irq.h"

static void *
main2(void *aux)
{
  int d = 0;
  while(1) {
    printf("hello in main2: %d\n", d);
    d++;
    syscall1(SYS_sleep, HZ);
  }
  return NULL;
}

static void *
main3(void *aux)
{
  printf("hello in main3\n");
  syscall1(SYS_sleep, 203);
  return NULL;
}



int
main(void)
{
  printf("Hello in main\n");

  task_create(main2, NULL, 256, "main2");
  task_create(main3, NULL, 256, "main3");

  printf("OK main exits\n");
}


