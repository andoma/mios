#include <mios/task.h>
#include <mios/mios.h>
#include <mios/cli.h>
#include <mios/sys.h>
#include <mios/eventlog.h>
#include <mios/version.h>
#include <stdio.h>
#include <string.h>
#include "irq.h"

extern unsigned long _init_array_begin;
extern unsigned long _init_array_end;
extern unsigned long _fini_array_begin;
extern unsigned long _fini_array_end;


int  __attribute__((weak))
main(void)
{
  cli_console('>');
  printf("No console input\n");
  return 0;
}

__attribute__((noinline))
static void
log_sysinfo(void)
{
  evlog(LOG_NOTICE, "System booted");

  const unsigned char *bid = mios_build_id();

  evlog(LOG_NOTICE, "Mios: %s Build: [%02x%02x%02x%02x]",
        mios_get_version(), bid[0], bid[1], bid[2], bid[3]);

  const char *appname = mios_get_app_name();
  if(appname[0]) {
    evlog(LOG_NOTICE, "App: %s (%s)", appname, mios_get_app_version());
  }
}

__attribute__((noreturn))
static void *
main_trampoline(void *opaque)
{
  log_sysinfo();
  main();
  thread_exit(0);
}


static void
call_array_fwd(void **p, void **end)
{
  while(p != end) {
    void (*fn)(void) = *p++;
    fn();
  }
}


static void
call_array_rev(void **p, void **start)
{
  while(p != start) {
    void (*fn)(void) = *--p;
    fn();
  }
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

  call_array_fwd((void *)&_init_array_begin, (void *)&_init_array_end);

  size_t stack_size = 0;
#ifdef MAIN_STACK_SIZE
  stack_size = MAIN_STACK_SIZE;
#endif
  task_create_shell(main_trampoline, NULL, "main", stack_size);
}


void  __attribute__((weak, noreturn))
halt(const char *msg)
{
  printf("%s\n", msg);
  while(1) {}
}

void
fini(void)
{
  call_array_rev((void *)&_fini_array_end, (void *)&_fini_array_begin);
}


void
panic(const char *fmt, ...)
{
  irq_forbid(IRQ_LEVEL_ALL);
  fini();

  thread_t *t = thread_current();
  printf("\n\nPANIC in %s: ", t ? t->t_name : "<nothread>");
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  cli_console('#');
  halt(fmt);
}


void
__assert_func(const char *expr, const char *file, int line)
{
  panic("ASSERT: %s at %s:%d\n", expr, file, line);
}


const struct flash_iface *   __attribute__((weak))
flash_get_primary(void)
{
  return NULL;
}

reset_reason_t __attribute__((weak))
sys_get_reset_reason(void)
{
  return 0;
}

const struct serial_number __attribute__((weak))
sys_get_serial_number(void)
{
  struct serial_number sn = {};
  return sn;
}

void  __attribute__((weak))
sys_watchdog_start(gpio_t blink)
{

}

void __attribute__((weak))
pbuf_data_add(void *start, void *end)
{
}

void  __attribute__((weak))
wakelock_acquire(void)
{

}

void  __attribute__((weak))
wakelock_release(void)
{

}
