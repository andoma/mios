#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

#include "heap.h"
#include "sys.h"
#include "task.h"
#include "timer.h"

//static volatile unsigned int * const UART0DR = (unsigned int *)0x4000c000;
static volatile unsigned int * const CPUID   = (unsigned int *)0xe000ed00;
static volatile unsigned int * const AIRCR   = (unsigned int *)0xe000ed0c;
//static volatile unsigned int * const ICSR    = (unsigned int *)0xe000ed04;
static volatile unsigned int * const HFSR    = (unsigned int *)0xe000ed2c;
static volatile unsigned int * const CFSR    = (unsigned int *)0xe000ed28;
static volatile unsigned int * const UFSR    = (unsigned int *)0xe000ed2a;
//static volatile unsigned int * const MMAR    = (unsigned int *)0xe000ed34;



static void  __attribute__ ((noreturn))
panic(const char *fmt, ...)
{
  printf("PANIC in %s: ", curtask ? curtask->t_name : "<notask>");
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  while(1) {}
}

#if 0

static void
uart_init(void)
{

}



static void
uart_putc(void *p, char c)
{
  *(int *)p = c;
}
#else

static volatile unsigned int * const UART_ENABLE   = (unsigned int *)0x40002500;
static volatile unsigned int * const UART_PSELTXD  = (unsigned int *)0x4000250c;
static volatile unsigned int * const UART_PSELRXD  = (unsigned int *)0x40002514;
static volatile unsigned int * const UART_TXD      = (unsigned int *)0x4000251c;
static volatile unsigned int * const UART_BAUDRATE = (unsigned int *)0x40002524;
static volatile unsigned int * const UART_TX_TASK  = (unsigned int *)0x40002008;
static volatile unsigned int * const UART_TX_RDY   = (unsigned int *)0x4000211c;

static void
uart_init(void)
{
  *UART_PSELTXD = 6;
  *UART_PSELRXD = 8;
  *UART_ENABLE = 4;
  *UART_BAUDRATE = 0x1d60000;
}

static void
uart_putc(void *p, char c)
{
  sys_forbid();
  *UART_TXD = c;
  *UART_TX_TASK = 1;
  while(!*UART_TX_RDY) {
  }
  *UART_TX_RDY = 0;
  *UART_TX_TASK = 0;
  sys_permit();
}

#endif

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

  uart_init();

  init_printf((unsigned int *)0x4000c000, uart_putc);

  void *heap_start = (void *)&_ebss;
  void *heap_end =   (void *)0x20008000;

  printf("Booting CPUID:0x%08x, edata:%p, ebss:%p, eheap:%p\n",
         *CPUID, &_edata, &_ebss, heap_end);
  printf("AIRCR: 0x%0x\n", *AIRCR);

  heap_init(heap_start, heap_end - heap_start);

  timer_init();

  extern void *main(void *);
  task_create(main, NULL, 256, "main");

}

void
exc_nmi(void)
{
  panic("NMI");
}

void
exc_hard_fault(void)
{
  panic("HARD FAULT, HFSR:0x%x CFSR:0x%x UFSR:0x%x\n",
        *HFSR, *CFSR, *UFSR);
}

void
exc_mm_fault(void)
{
  panic("MM");
}

void
exc_bus_fault(void)
{
  panic("Bus");
}
void
exc_usage_fault(void)
{
  panic("Usage");
}

void
exc_reserved(void)
{
  panic("Res");
}

void
__assert_func(const char *expr, const char *file, int line)
{
  panic("ASSERT: %s at %s:%d\n", expr, file, line);
}
