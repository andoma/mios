#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <malloc.h>

#include "net/pbuf.h"   // pbuf_set_data_size()

#include <mios/mios.h>
#include <mios/task.h>

#include "cpu.h"
#include "irq.h"
#include "linux.h"
#include "sim.h"
#include <unistd.h>   /* clock_get */

struct cpu cpu0;

static void *idle_sp;

// ---- Library mode --------------------------------------------------------
// mios built as a shared object, boot-to-idle and resumed cooperatively by
// a host harness on the harness's own thread (no signals, virtual time).
// The harness and the mios kernel are one OS thread, swapped by
// cpu_coswitch().  See src/platform/hostlib.
// These survive init()'s .bss clear (the forward coswitch saves
// g_harness_sp before init() runs), so keep them in .data like
// host_boot_sp.
int host_lib_mode __attribute__((section(".data")));
static void *g_harness_sp __attribute__((section(".data")));
static void *g_mios_sp __attribute__((section(".data")));
static uint64_t g_lib_target __attribute__((section(".data")));

void cpu_coswitch(void **save_sp, void *resume_sp);

// timer.c (virtual clock helpers, also used by sim.c)
uint64_t host_timer_next(void);
void host_timer_fire(void);
void host_vclock_set(uint64_t now);

extern volatile uint32_t irq_pending;
void irq_dispatch(void);
static void cpu_idle_entry(void);

// One idle turn in library mode. Mirrors sim_idle(): dispatch a pending
// IRQ and let threads run, else advance the virtual clock toward the
// harness's step target, firing due timers; when nothing is due before
// the target, hand the CPU back to the harness until the next step.
static void
lib_idle(void)
{
  if(__atomic_load_n(&irq_pending, __ATOMIC_SEQ_CST)) {
    irq_dispatch();
    return;
  }
  uint64_t nt = host_timer_next();
  if(nt <= g_lib_target) {
    host_vclock_set(nt);
    host_timer_fire();   // pends the timer IRQ, dispatched next turn
    return;
  }
  host_vclock_set(g_lib_target);
  cpu_coswitch(&g_mios_sp, g_harness_sp);   // back to harness; resumes on step
}

static void
mios_lib_entry(void)
{
  extern void init(void);
  init();                                   // clears bss, runs ctors, starts main
  cpu_jump_stack(idle_sp, cpu_idle_entry);  // enter the (library) idle loop
}

// Exported (via the platform ABI) boot + step.
void
host_lib_boot(void)
{
  host_lib_mode = 1;
  host_vtime = 1;      // virtual time; no signal-driven timers

  const size_t ss = 1 << 20;
  uint8_t *stk = linux_mmap(NULL, ss, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  // Land in mios_lib_entry with the SysV entry contract rsp%16==8 (as if
  // reached by a call). After cpu_coswitch pops 6 saved regs and rets, rsp
  // equals this base, so make the base %16==8.
  uint64_t *sp = (uint64_t *)(((uintptr_t)(stk + ss) & ~(uintptr_t)15) - 8);
  *--sp = (uint64_t)mios_lib_entry;   // return address for the first coswitch
  *--sp = 0;                          // rbp
  *--sp = 0;                          // rbx
  *--sp = 0;                          // r12
  *--sp = 0;                          // r13
  *--sp = 0;                          // r14
  *--sp = 0;                          // r15
  g_mios_sp = sp;
  cpu_coswitch(&g_harness_sp, g_mios_sp);   // run mios to first idle, then return
}

void
host_lib_step(uint64_t dt_us)
{
  g_lib_target = clock_get() + dt_us;
  cpu_coswitch(&g_harness_sp, g_mios_sp);
}

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
    if(host_lib_mode) {
      lib_idle();
    } else if(host_vtime) {
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


// Pbuf buffer size this suite wants, 0 for the platform default. Asked
// before init() because the pool is carved during it, so a suite cannot
// choose its own size once it is running.
int __attribute__((weak))
host_platform_pbuf_data_size(const char *suite)
{
  return 0;
}


void __attribute__((noreturn))
host_start(long *sp)
{
  host_boot_sp = sp;
  const char *suite = host_positional(0);
  if(suite != NULL) {
    host_vtime = host_platform_vtime(suite);
#ifdef ENABLE_PBUF_DYNAMIC_SIZE
    const int pbuf_size = host_platform_pbuf_data_size(suite);
    if(pbuf_size)
      pbuf_set_data_size(pbuf_size);
#endif
  }
  host_irq_init();
  init();
  cpu_jump_stack(idle_sp, cpu_idle_entry);
}


// Map an anonymous heap and hand it to the allocator. Shared by the host
// packagings (executable and shared object); each provides its own tiny
// constructor that picks the size.
void
host_map_heap(size_t size)
{
  void *heap = linux_mmap(NULL, size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if(heap == NULL || heap == (void *)-1)
    panic("host: unable to map heap");
  heap_add_mem((long)heap, (long)heap + size,
               MEM_TYPE_LOCAL | MEM_TYPE_DMA, 10);
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


// Test hook: when set, reboot() calls this instead of re-exec'ing the
// process. Lets a virtual-time suite observe an OTA-triggered reboot
// (svc_ota calls reboot() on success) without tearing down the harness.
// See platform/host/suite_ota.c. The hook must not return.
void (*host_test_reboot_hook)(void);

void
reboot(void)
{
  if(host_test_reboot_hook != NULL)
    host_test_reboot_hook();   // does not return

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
