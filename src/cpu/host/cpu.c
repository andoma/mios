#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <malloc.h>

#include <mios/mios.h>
#include <mios/task.h>

#include "cpu.h"
#include "irq.h"
#include "linux.h"
#include "sim.h"

struct cpu cpu0;

static void *idle_sp;

// Captured before init() clears .bss, hence in .data
static long *host_boot_sp __attribute__((section(".data")));

void init(void);
void host_irq_init(void);

static void __attribute__((constructor(150)))
cpu_init(void)
{
  const size_t stack_size = MIN_STACK_SIZE;

  void *sp_bottom = xalloc(stack_size + sizeof(thread_t),
                           CPU_STACK_ALIGNMENT, 0);
  memset(sp_bottom, 0x55, stack_size + sizeof(thread_t));
  void *sp = sp_bottom + stack_size;

  thread_t *t = sp;
  strlcpy(t->t_name, "idle", sizeof(t->t_name));
  t->t_sp_bottom = sp_bottom;
  t->t_stream = NULL;
  t->t_task.t_state = TASK_STATE_ZOMBIE;
  t->t_task.t_prio = 0;
  sched_cpu_init(&curcpu()->sched, t);

  idle_sp = sp;
}


/**
 * See entry.S for the frame layout.
 */
void *
cpu_stack_init(uint64_t *stack, void *entry,
               void (*thread_exit)(void *), int nargs, va_list ap)
{
  uint64_t args[4] = {};
  for(int i = 0; i < nargs && i < 4; i++)
    args[i] = va_arg(ap, uintptr_t);

  uint64_t *p = (uint64_t *)((uintptr_t)stack & ~15);
  *--p = (uint64_t)cpu_thread_start;  // return address
  *--p = (uint64_t)thread_exit;       // rbp
  *--p = (uint64_t)entry;             // rbx
  *--p = args[0];                     // r12
  *--p = args[1];                     // r13
  *--p = args[2];                     // r14
  *--p = args[3];                     // r15
  return p;
}


// Called by cpu_thread_start (entry.S) on a new thread's stack, still
// logically inside the switch "exception". Return to thread mode.
void
cpu_thread_start_hook(void)
{
  irq_depth = 0;
  irq_level = 0;
  irq_barrier();
  if(__atomic_load_n(&irq_pending, __ATOMIC_SEQ_CST))
    irq_dispatch();
}


void __attribute__((noreturn))
cpu_idle(void)
{
  while(1) {
    if(host_vtime) {
      sim_idle();
    } else {
      linux_sigsuspend_all();
    }
  }
}


static void
cpu_idle_entry(void)
{
  // Equivalent of "cpsie i" in the cortexm entry code
  irq_depth = 0;
  irq_level = 0;
  irq_barrier();
  irq_dispatch();
  cpu_idle();
}


int __attribute__((weak))
host_platform_vtime(const char *suite)
{
  return 0;
}


void __attribute__((noreturn))
host_start(long *sp)
{
  host_boot_sp = sp;
  const char *suite = host_positional(0);
  if(suite != NULL)
    host_vtime = host_platform_vtime(suite);
  host_irq_init();
  init();
  cpu_jump_stack(idle_sp, cpu_idle_entry);
}


void __attribute__((noreturn))
host_exit(int code)
{
  irq_off();
  fini();
  linux_exit_group(code);
}


void
halt(const char *msg)
{
  linux_exit_group(1);
}


void
reboot(void)
{
  irq_off();
  fini();

  const long argc = host_boot_sp[0];
  char **argv = (char **)(host_boot_sp + 1);
  char **envp = argv + argc + 1;
  linux_syscall(SYS_execve, "/proc/self/exe", argv, envp);
  linux_exit_group(1);
}


// ---- Command line / environment ----

static char **
host_argv(void)
{
  return (char **)(host_boot_sp + 1);
}

char **
host_envp(void)
{
  return host_argv() + host_boot_sp[0] + 1;
}

const char *
host_arg(const char *name)
{
  const size_t len = strlen(name);
  char **argv = host_argv();
  for(int i = 1; argv[i] != NULL; i++) {
    const char *a = argv[i];
    if(!strcmp(a, "--"))
      break;
    if(a[0] != '-' || a[1] != '-' || strncmp(a + 2, name, len))
      continue;
    if(a[2 + len] == '=')
      return a + 3 + len;
    if(a[2 + len] != 0)
      continue;
    if(argv[i + 1] != NULL && argv[i + 1][0] != '-')
      return argv[i + 1];
    return "";
  }
  return NULL;
}

const char *
host_positional(int n)
{
  char **argv = host_argv();
  for(int i = 1; argv[i] != NULL; i++) {
    const char *a = argv[i];
    if(!strcmp(a, "--"))
      break;
    if(a[0] == '-') {
      // "--name value" consumes the following non-dash word, mirror host_arg()
      if(a[1] == '-' && strchr(a, '=') == NULL &&
         argv[i + 1] != NULL && argv[i + 1][0] != '-')
        i++;
      continue;
    }
    if(n-- == 0)
      return a;
  }
  return NULL;
}

char **
host_argv_tail(void)
{
  char **argv = host_argv();
  int i;
  for(i = 1; argv[i] != NULL; i++) {
    if(!strcmp(argv[i], "--"))
      return argv + i + 1;
  }
  return argv + i; // Points at the terminating NULL
}

const char *
host_env(const char *name)
{
  const size_t len = strlen(name);
  for(char **e = host_envp(); *e != NULL; e++) {
    if(!strncmp(*e, name, len) && (*e)[len] == '=')
      return *e + len + 1;
  }
  return NULL;
}


void
dcache_op(void *addr, size_t size, uint32_t flags)
{
}


void
icache_invalidate(void)
{
}
