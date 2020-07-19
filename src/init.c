#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

#include "heap.h"
#include "sys.h"
#include "task.h"
#include "timer.h"
#include "irq.h"
#include "cpu.h"
#include "mios.h"

#include "platform.h"

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

  extern void *main(void *);
  task_create(main, NULL, 512, "main", TASK_FPU);
}



void
panic(const char *fmt, ...)
{
  irq_off();
  task_t *t = task_current();
  printf("PANIC in %s: ", t ? t->t_name : "<notask>");
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
