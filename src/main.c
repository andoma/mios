#include <stdio.h>
#include <assert.h>

#include "task.h"
#include "mios.h"

int
main(void)
{
  printf("Welcome to main()\n");
  printf("You can type freely here: ");
  while(1) {
    int c = getchar();
    if(c == -1)
      break;
    printf("%c", c);
  }

  printf("No console input\n");

  return 0;
}


