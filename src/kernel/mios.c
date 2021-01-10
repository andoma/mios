#include <mios/task.h>
#include <mios/mios.h>
#include <mios/cli.h>
#include <stdio.h>
#include <string.h>
#include "irq.h"


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
  extern unsigned long _sdata;
  extern unsigned long _etext;
  extern unsigned long _edata;
  memcpy(&_sdata, &_etext, (void *)&_edata - (void *)&_sdata);

  extern unsigned long _sbss;
  extern unsigned long _ebss;
  memset(&_sbss, 0, (void *)&_ebss - (void *)&_sbss);


  extern unsigned long _init_array_begin;
  extern unsigned long _init_array_end;

  void **init_array_begin = (void *)&_init_array_begin;
  void **init_array_end = (void *)&_init_array_end;

  while(init_array_begin != init_array_end) {
    void (*init)(void) = *init_array_begin;
    init();
    init_array_begin++;
  }

  task_create((void *)&main, NULL, 1024, "main", TASK_FPU | TASK_DETACHED, 0);
}



void  __attribute__((weak))
platform_panic(void)
{
}


void
panic(const char *fmt, ...)
{
  irq_forbid(IRQ_LEVEL_ALL);
  platform_panic();
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


