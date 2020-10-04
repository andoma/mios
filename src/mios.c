#include <stdio.h>
#include "irq.h"
#include "task.h"
#include "mios.h"
#include "cli.h"


int  __attribute__((weak))
main(void)
{
  printf("Welcome to Mios default main()\n");
  cli_console();
  printf("No console input\n");
  return 0;
}


void
init(void)
{
  extern unsigned long _init_array_begin;
  extern unsigned long _init_array_end;

  void **init_array_begin = (void *)&_init_array_begin;
  void **init_array_end = (void *)&_init_array_end;

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
  irq_forbid(IRQ_LEVEL_ALL);
  task_t *t = task_current();
  printf("\n\nPANIC in %s: ", t ? t->t_name : "<notask>");
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  cli_console();
  printf("Halted\n");
  while(1) {}
}


void
__assert_func(const char *expr, const char *file, int line)
{
  panic("ASSERT: %s at %s:%d\n", expr, file, line);
}


