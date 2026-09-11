#pragma once

#include <stdint.h>
#include <stdarg.h>

#include <mios/task.h>

#define CPU_STACK_ALIGNMENT 16

// Signal frames land on the interrupted thread's stack, nested up to
// the number of IRQ levels: ~3.5kB each on x86-64 (with AVX-512 state),
// ~4.8kB on aarch64 (siginfo plus a ucontext with 4kB reserved for
// FP/SIMD and SVE records).
#if defined(__aarch64__)
#define MIN_STACK_SIZE 49152
#else
#define MIN_STACK_SIZE 32768
#endif

typedef struct cpu {
  sched_cpu_t sched;
} cpu_t;

static inline void
cpu_fpu_enable(int on)
{
}

static inline void
cpu_stack_redzone(thread_t *t)
{
}

static inline uint32_t
cpu_cycle_counter(void)
{
#if defined(__x86_64__)
  uint32_t lo, hi;
  asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return lo;
#else
  // Not a cycle counter, but the same thing we want it for: a cheap
  // monotonic tick with no syscall. Readable from EL0 on Linux.
  uint64_t cnt;
  asm volatile ("mrs %0, cntvct_el0" : "=r"(cnt));
  return cnt;
#endif
}

// Terminate the process: run destructors (restores the terminal) and exit
void host_exit(int code) __attribute__((noreturn));

// Test-only reboot hook (see cpu.c / platform/host/suite_ota.c). When
// non-NULL, reboot() calls it instead of re-exec'ing.
extern void (*host_test_reboot_hook)(void);

void cpu_switch(void);

void cpu_jump_stack(void *sp, void (*fn)(void)) __attribute__((noreturn));

void cpu_thread_start(void);

// ---- Machine specific (arch_${arch}.c, entry_${arch}.S) ----

struct stream;
struct linux_ucontext;

// Build a thread's initial context switch frame below stack (which
// grows down), so that resuming it enters entry(arg0, ...) and passes
// the result to thread_exit()
void *cpu_stack_init(uint64_t *stack, void *entry,
                     void (*thread_exit)(void *), int nargs, va_list ap);

// Same, for cpu_coswitch(): resuming the returned sp calls fn()
void *cpu_coswitch_frame_init(void *stack_top, void (*fn)(void));

// Panic output: the interrupted register state, and any nested signal
// frames found between sp and top
void host_regs_print(struct stream *st, const struct linux_ucontext *uc);

void host_sigframes_print(struct stream *st, const uintptr_t *sp,
                          const uintptr_t *top);

// ---- Virtual time (see timer.c) ----

extern int host_vtime;         // 1: clock only advances when idle

uint64_t host_timer_next(void);     // next Mios timer deadline, UINT64_MAX if none
void host_vclock_set(uint64_t now); // advance the virtual clock (never backwards)
void host_timer_fire(void);         // raise the timer IRQ

// Platform hook: should this test suite run in virtual time? (weak, 0)
int host_platform_vtime(const char *suite);

// Switch rand() to a deterministic sequence (see rnd.c)
void host_rand_seed(uint32_t seed);

// ---- Command line / environment (see cpu.c) ----

// n:th positional argument (not starting with '-', before "--"), or NULL.
// The first one names a test suite, see platform/host/hosttest.c
const char *host_positional(int n);

static inline int host_test_mode(void) { return host_positional(0) != NULL; }

// "--name=value" or "--name value" -> value, "--name" alone -> "",
// absent -> NULL. Parsing stops at "--".
const char *host_arg(const char *name);

// Arguments after "--", NULL terminated (empty list if none)
char **host_argv_tail(void);

const char *host_env(const char *name);

char **host_envp(void);

// Map a size-byte anonymous heap and register it with the allocator.
void host_map_heap(size_t size);
