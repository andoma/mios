#include <stdio.h>
#include <assert.h>
#include "sys.h"
#include "task.h"


static void *
main2(void *aux)
{
  int d = 0;
  while(1) {
    syscall1(SYS_sleep, HZ);
    printf("hello in main2: %d\n", d);
    d++;
  }
  return NULL;
}

static void *
main3(void *aux)
{
  return NULL;
  while(1) {
    syscall1(SYS_sleep, 203);
    printf("hello in main3\n");
  }
  return NULL;
}



int
main(void)
{
  printf("Hello in main\n");
  task_create(main2, NULL, 256, "main2");
  task_create(main3, NULL, 256, "main3");

  syscall1(SYS_sleep, 3000);

  printf("OK main exits\n");
}


