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

#include "platform.h"

void
init(void)
{
  extern unsigned long _sbss;
  extern unsigned long _sdata;
  extern unsigned long _etext;
  extern unsigned long _ebss;
  extern unsigned long _edata;

  unsigned long *src, *dst;

  src = &_etext;
  dst = &_sdata;
  while(dst < &_edata)
    *dst++ = *src++;

  src = &_sbss;
  while(src < &_ebss)
    *src++ = 0;

  platform_console_init_early();

  void *heap_start = (void *)&_ebss;
  void *heap_end =   platform_heap_end();

  printf("RAM Layout edata:%p, ebss:%p, eheap:%p\n",
         &_edata, &_ebss, heap_end);

  heap_init(heap_start, heap_end - heap_start);

  timer_init();

  extern void *main(void *);
  task_create(main, NULL, 256, "main");

  irq_init();
}



void
panic(const char *fmt, ...)
{
  printf("PANIC in %s: ", curtask ? curtask->t_name : "<notask>");
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  while(1) {
  }
}


void
__assert_func(const char *expr, const char *file, int line)
{
  panic("ASSERT: %s at %s:%d\n", expr, file, line);
}
