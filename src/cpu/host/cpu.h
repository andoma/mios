#pragma once

#include <stdint.h>
#include <stdarg.h>

#include <mios/task.h>

#define CPU_STACK_ALIGNMENT 16

// Signal frames (with AVX-512 state) are ~3.5kB each and land on the
// interrupted thread's stack, nested up to the number of IRQ levels.
#define MIN_STACK_SIZE 32768

void *cpu_stack_init(uint64_t *stack, void *entry,
                     void (*thread_exit)(void *), int nargs, va_list ap);

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
  uint32_t lo, hi;
  asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return lo;
}

// Terminate the process: run destructors (restores the terminal) and exit
void host_exit(int code) __attribute__((noreturn));

void cpu_switch(void);

void cpu_jump_stack(void *sp, void (*fn)(void)) __attribute__((noreturn));

void cpu_thread_start(void);

// ---- Command line / environment (see cpu.c) ----

// "--name=value" or "--name value" -> value, "--name" alone -> "",
// absent -> NULL. Parsing stops at "--".
const char *host_arg(const char *name);

// Arguments after "--", NULL terminated (empty list if none)
char **host_argv_tail(void);

const char *host_env(const char *name);

char **host_envp(void);
