#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "task.h"
#include "mios.h"

int  __attribute__((weak))
main(void)
{
  printf("Welcome to Mios default main()\n");
  printf("Echo console> ");
  while(1) {
    int c = getchar();
    if(c < 0)
      break;
    printf("%c", c);
  }

  printf("No console input\n");

  return 0;
}


