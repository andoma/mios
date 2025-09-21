#include <mios/task.h>
#include <mios/mios.h>
#include <mios/cli.h>
#include <mios/sys.h>
#include <mios/eventlog.h>
#include <mios/version.h>
#include <mios/hostname.h>
#include <mios/ghook.h>

#include <stdio.h>
#include <string.h>
#include "irq.h"
#include "cpu.h"
extern unsigned long _init_array_begin;
extern unsigned long _init_array_end;
extern unsigned long _fini_array_begin;
extern unsigned long _fini_array_end;

#ifdef APPNAME
char hostname[HOSTNAME_BUFFER_SIZE] = APPNAME;
#else
char hostname[HOSTNAME_BUFFER_SIZE];
#endif
mutex_t hostname_mutex;

void
hostname_setf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  mutex_lock(&hostname_mutex);
  vsnprintf(hostname, sizeof(hostname), fmt, ap);
  mutex_unlock(&hostname_mutex);
  va_end(ap);
}

void
hostname_set(const char *name)
{
  hostname_setf("%s", name);
}


int  __attribute__((weak))
main(void)
{
  cli_console('>');
  printf("No console input\n");
  return 0;
}


static void __attribute__((constructor(4999)))
multitasking_mark(void)
{
  // Without something the compiler might just remove this completetly
  asm volatile("nop" ::: "memory");
}


static void
call_array_fwd(void **p, void **end, int en)
{
  while(p != end) {
    void (*fn)(void) = *p++;
    if(fn == &multitasking_mark) {
      en = !en;
      continue;
    }
    if(en)
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


__attribute__((noreturn))
static void *
main_trampoline(void *opaque)
{
  call_array_fwd((void *)&_init_array_begin, (void *)&_init_array_end, 0);
  main();
  thread_exit(0);
}


void
init(void)
{
  extern unsigned long _sbss;
  extern unsigned long _ebss;
  memset(&_sbss, 0, (void *)&_ebss - (void *)&_sbss);

#ifdef HAVE_FPU
  cpu_fpu_enable(1);
#endif

  call_array_fwd((void *)&_init_array_begin, (void *)&_init_array_end, 1);

#ifdef HAVE_FPU
  cpu_fpu_enable(0);
#endif

  log_sysinfo();

  thread_create_shell(main_trampoline, NULL, "main", stdio);
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



__attribute__((weak))
stream_t *
get_panic_stream(void)
{
  return stdio;
}

void
panic(const char *fmt, ...)
{
  irq_off();
  fini();

  thread_t *t = thread_current();

  stream_t *st = get_panic_stream();
  stprintf(st, "\n\nPANIC in %s: ", t ? t->t_name : "<nothread>");
  va_list ap;
  va_start(ap, fmt);
  vstprintf(st, fmt, ap);
  va_end(ap);
  stprintf(st, "\n");
  stream_write(st, NULL, 0, 0); // Stream flush
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


void
shutdown_notification(const char *reason)
{
  ghook_invoke(GHOOK_SYSTEM_SHUTDOWN, reason);
}
