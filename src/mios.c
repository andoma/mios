#include <stdio.h>
#include "irq.h"
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


void
init(void)
{
  extern unsigned long _init_array;
  extern unsigned long _etext;

  void **init_array_begin = (void *)&_init_array;
  void **init_array_end = (void *)&_etext;

  while(init_array_begin != init_array_end) {
    void (*init)(void) = *init_array_begin;
    init();
    init_array_begin++;
  }

  task_create((void *)&main, NULL, 1024, "main", TASK_FPU, 0);
}



void
panic(const char *fmt, ...)
{
  irq_off();
  task_t *t = task_current();
  printf("\n\nPANIC in %s: ", t ? t->t_name : "<notask>");
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  while(1) {
  }
}


void
__assert_func(const char *expr, const char *file, int line)
{
  panic("ASSERT: %s at %s:%d\n", expr, file, line);
}


