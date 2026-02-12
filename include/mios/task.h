/**
 * @file task.h
 * @brief MIOS Task and Thread Management
 *
 * This header provides the core task and thread management facilities for MIOS,
 * including:
 * - Thread creation, execution, and joining
 * - Priority-based preemptive scheduling
 * - Mutual exclusion (mutexes)
 * - Condition variables for thread synchronization
 * - Task sleep/wakeup primitives
 * - Software interrupt (softirq) support
 *
 * The scheduler supports 32 priority levels (0-31), where higher numbers
 * indicate higher priority. Priority 31 is the highest priority.
 * Priority 0 is reserved for the idle task only and should not be used
 * by application code.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>

#include "error.h"

STAILQ_HEAD(task_queue, task);
LIST_HEAD(task_list, task);

/**
 * Number of priority levels supported by the scheduler
 */
#define TASK_PRIOS 32

/**
 * Mask for extracting priority from task fields
 */
#define TASK_PRIO_MASK (TASK_PRIOS - 1)

/**
 * Task state values
 */
#define TASK_STATE_NONE     0  /**< No state/uninitialized */
#define TASK_STATE_RUNNING  1  /**< Currently executing */
#define TASK_STATE_READY    2  /**< Ready to run */
#define TASK_STATE_SLEEPING 3  /**< Sleeping on a waitable */
#define TASK_STATE_ZOMBIE   4  /**< Terminated, waiting to be joined */
#define TASK_STATE_MUXED_SLEEP 5  /**< Sleeping in a multiplexed wait */

/**
 * Base task structure
 *
 * Represents a schedulable unit of execution. Can be either a lightweight
 * task with a run function, or part of a full thread with its own stack.
 */
typedef struct task {
  union {
    struct {
      STAILQ_ENTRY(task) t_ready_link;  /**< Link in ready queue */
      void (*t_run)(struct task *t);    /**< Task run function */
    };
    LIST_ENTRY(task) t_wait_link;       /**< Link in wait list */
  };

  uint8_t t_flags;   /**< Task flags (TASK_THREAD, TASK_DETACHED, etc.) */
  uint8_t t_prio;    /**< Task priority (0-31, higher is higher priority, 0=idle only) */
  uint8_t t_state;   /**< Current task state (TASK_STATE_*) */
  uint8_t t_index;   /**< Task index for internal use */
} task_t;

/**
 * Schedule a task for execution
 *
 * @param t  Pointer to the task to be scheduled
 *
 * @note This function can only be called from IRQ_LEVEL_SCHED and below.
 *       For execution from higher interrupt levels, use softirq_raise() instead.
 */
void task_run(task_t *t);

/*
 * Per thread memory block
 *
 * ----------------- <- sp_bottom
 * | REDZONE       |
 * -----------------
 * | Normal stack  |
 * ----------------- <- initial sp points here
 * | FPU save area | (Optional)
 * -----------------
 * | struct task   |
 * -----------------
 *
 */


/**
 * Full thread structure with stack and execution context
 *
 * Extends the base task_t structure with a stack and additional
 * thread-specific information.
 */
typedef struct thread {
  task_t t_task;  /**< Base task structure (must be first) */

  void *t_sp_bottom;  /**< Bottom of stack (for overflow detection) */
  void *t_sp;         /**< Current stack pointer */
#ifdef HAVE_FPU
  void *t_fpuctx;     /**< FPU context pointer; NULL if FPU not allowed */
#endif

#ifdef ENABLE_TASK_WCHAN
  const char *t_wchan;  /**< Wait channel name (valid when SLEEPING) */
#endif

  SLIST_ENTRY(thread) t_global_link;  /**< Link in global thread list */
  struct stream *t_stream;            /**< Associated I/O stream */

  char t_name[11];      /**< Thread name (max 10 chars + null) */
  uint8_t t_refcount;   /**< Reference count for thread lifecycle */

#ifdef ENABLE_TASK_ACCOUNTING
  uint32_t t_cycle_enter;       /**< CPU cycle count at context switch in */
  uint32_t t_cycle_acc;         /**< Accumulated CPU cycles */
  uint32_t t_ctx_switches_acc;  /**< Accumulated context switches */
  uint32_t t_ctx_switches;      /**< Current context switch count */
  uint16_t t_load;              /**< CPU load percentage (0-10000 = 0-100%) */
  uint16_t t_stacksize;         /**< Stack size in bytes */
#endif

} thread_t;

/**
 * Per-CPU scheduler structure
 *
 * Maintains the scheduling state for a single CPU, including the current
 * running thread, idle task, and ready queues for each priority level.
 */
typedef struct sched_cpu {
  thread_t *current;      /**< Currently executing thread */
  task_t *idle;           /**< Idle task (runs when no other tasks ready) */
  uint32_t active_queues; /**< Bitmask of non-empty ready queues */
  struct task_queue readyqueue[TASK_PRIOS];  /**< Ready queue per priority */

#ifdef HAVE_FPU
  thread_t *current_fpu;  /**< Thread currently owning FPU context */
#endif

} sched_cpu_t;


/**
 * Initialize a CPU scheduler structure
 *
 * @param sc    Pointer to the scheduler CPU structure to initialize
 * @param idle  Pointer to the idle thread for this CPU
 */
void sched_cpu_init(sched_cpu_t *sc, thread_t *idle);

/**
 * Waitable object for task synchronization
 *
 * Tasks can sleep on a waitable and be woken up by other tasks.
 * Used as the basis for mutexes, condition variables, and other
 * synchronization primitives.
 */
typedef struct task_waitable {
  struct task_list list;  /**< List of tasks waiting on this object */
#ifdef ENABLE_TASK_WCHAN
  const char *name;       /**< Name for debugging purposes */
#endif
} task_waitable_t;


#ifdef ENABLE_TASK_WCHAN
#define task_waitable_init(t, n) do { LIST_INIT(&(t)->list); (t)->name = n; } while(0)
#define WAITABLE_INITIALIZER(n) { .name = (n)}
#else
#define task_waitable_init(t, n) LIST_INIT(&(t)->list)
#define WAITABLE_INITIALIZER(n) {}

#endif


/**
 * Mutex for mutual exclusion
 *
 * Provides exclusive access to shared resources. The union allows for
 * fast atomic operations when uncontended, falling back to a waiter
 * list when threads need to block.
 */
typedef struct mutex {
  union {
    task_waitable_t waiters;  /**< List of waiting threads */
    intptr_t lock;            /**< Lock value (0=unlocked, 1=locked) */
  };
} mutex_t;

/**
 * Condition variable for thread synchronization
 *
 * Allows threads to wait for certain conditions to become true.
 * Must be used in conjunction with a mutex.
 */
typedef task_waitable_t cond_t;

/**
 * Initialize the current CPU's threading subsystem
 *
 * @param sc         Pointer to the scheduler CPU structure for this CPU
 * @param cpu_name   Name of the CPU for identification purposes
 * @param sp_bottom  Bottom of the stack pointer for the initial thread
 */
void thread_init_cpu(sched_cpu_t *sc, const char *cpu_name, void *sp_bottom);


// Flag bits 0-7 is stored in task->t_flags
#define TASK_THREAD    0x1  // A full thread with stack
#define TASK_DETACHED  0x2  // Should be auto-joined by system on thread_exit

// Remaining flags are only used during thread_create
#define TASK_NO_FPU            0x100
#define TASK_NO_DMA_STACK      0x200
#define TASK_MEMTYPE_SHIFT     16

/**
 * Create a new thread
 *
 * @param entry       Thread entry point function
 * @param arg         Argument to pass to the entry function
 * @param stack_size  Size of the stack to allocate for the thread (in bytes)
 * @param name        Name of the thread (max 10 characters, will be truncated)
 * @param flags       Thread creation flags:
 *                    - TASK_THREAD: Full thread with stack
 *                    - TASK_DETACHED: Auto-joined by system on thread_exit
 *                    - TASK_NO_FPU: Thread is not allowed to use FPU
 *                    - TASK_NO_DMA_STACK: Don't use DMA-capable memory
 *                                         for stack
 *
 *                    - Memory type can be specified in bits 16+
 * @param prio        Thread priority (1-31, where 31 is highest;
 *                                     0 is reserved for idle)
 *
 * @return Pointer to the created thread structure, or NULL on failure
 */
thread_t *thread_create(void *(*entry)(void *arg), void *arg, size_t stack_size,
                        const char *name, int flags, unsigned int prio);

/**
 * Exit the current thread
 *
 * @param ret  Return value to be retrieved by thread_join()
 *
 * @note This function does not return. The thread will be terminated
 *       and the return value made available to joining threads.
 */
void thread_exit(void *ret) __attribute__((noreturn));

/**
 * Wait for a thread to terminate and retrieve its return value
 *
 * @param t  Pointer to the thread to join
 *
 * @return The return value passed to thread_exit() by the joined thread
 *
 * @note This function blocks until the specified thread terminates.
 *       The thread resources are freed after join completes.
 */
void *thread_join(thread_t *t);

/**
 * Wake up one or more tasks waiting on a waitable object
 *
 * @param waitable  Pointer to the waitable object
 * @param all       If non-zero, wake all waiting tasks;
 *                  If zero, wake only one
 *
 * @note This function handles scheduler locking internally
 */
void task_wakeup(task_waitable_t *waitable, int all);

/**
 * Wake up one or more tasks waiting on a waitable object (scheduler
 * already locked)
 *
 * @param waitable  Pointer to the waitable object
 * @param all       If non-zero, wake all waiting tasks; if zero, wake only one
 *
 * @note Use this version when IRQ_LEVEL_SCHED is already forbidden
 * via irq_forbid()
 */
void task_wakeup_sched_locked(task_waitable_t *waitable, int all);

/**
 * Put the current task to sleep on a waitable object
 *
 * @param waitable  Pointer to the waitable object to sleep on
 *
 * @note The task will remain asleep until woken by task_wakeup().
 *       This function handles scheduler locking internally.
 */
void task_sleep(task_waitable_t *waitable);

/**
 * Put the current task to sleep on a waitable object (scheduler already locked)
 *
 * @param waitable  Pointer to the waitable object to sleep on
 *
 * @note Use this version when IRQ_LEVEL_SCHED is already forbidden
 * via irq_forbid()
 */
void task_sleep_sched_locked(task_waitable_t *waitable);

/**
 * Put the current task to sleep until a deadline or wakeup event
 *
 * @param waitable  Pointer to the waitable object to sleep on
 * @param deadline  Absolute deadline timestamp (in microseconds)
 *
 * @return 1 if the deadline expired (timeout), 0 if woken by task_wakeup()
 *
 * @note The task will wake either when task_wakeup() is called or when
 *       the deadline is reached, whichever comes first.
 */
int task_sleep_deadline(task_waitable_t *waitable, int64_t deadline)
  __attribute__((warn_unused_result));

/**
 * Put the current task to sleep for a relative time period or until wakeup
 *
 * @param waitable  Pointer to the waitable object to sleep on
 * @param useconds  Number of microseconds to sleep
 *
 * @return 1 if the timeout expired, 0 if woken by task_wakeup()
 *
 * @note The task will wake either when task_wakeup() is called or when
 *       the timeout period elapses, whichever comes first.
 */
int task_sleep_delta(task_waitable_t *waitable, int useconds)
  __attribute__((warn_unused_result));

/**
 * Get the currently executing thread
 *
 * @return Pointer to the current thread structure
 */
thread_t *thread_current(void);

#ifdef ENABLE_TASK_WCHAN
#define MUTEX_INITIALIZER(n) { .waiters = {.name = (n)}}
#else
#define MUTEX_INITIALIZER(n) {}
#endif


/**
 * Initialize a mutex
 *
 * @param m     Pointer to the mutex to initialize
 * @param name  Name of the mutex for debugging (only used if ENABLE_TASK_WCHAN is defined)
 */
inline void  __attribute__((always_inline))
mutex_init(mutex_t *m, const char *name)
{
  LIST_INIT(&m->waiters.list);
#ifdef ENABLE_TASK_WCHAN
  m->waiters.name = name;
#endif
}

/**
 * Acquire a mutex lock (slow path)
 *
 * @param m  Pointer to the mutex to lock
 *
 * @note This is the slow path implementation called by mutex_lock() when
 *       the fast atomic path fails. Applications should use mutex_lock() instead.
 */
void mutex_lock_slow(mutex_t *m);

/**
 * Acquire a mutex lock
 *
 * @param m  Pointer to the mutex to lock
 *
 * @note This function blocks until the mutex is acquired. It first attempts
 *       a fast atomic compare-exchange, falling back to mutex_lock_slow() if needed.
 */
inline void  __attribute__((always_inline))
mutex_lock(mutex_t *m)
{
  if(__atomic_always_lock_free(sizeof(intptr_t), 0)) {
    intptr_t expected = 0;
    if(__builtin_expect(__atomic_compare_exchange_n(&m->lock, &expected, 1, 1,
                                                    __ATOMIC_SEQ_CST,
                                                    __ATOMIC_RELAXED), 1))
      return;
  }
  mutex_lock_slow(m);
}


/**
 * Attempt to acquire a mutex lock without blocking (slow path)
 *
 * @param m  Pointer to the mutex to try locking
 *
 * @return 0 if the mutex was successfully locked, non-zero if already locked
 *
 * @note This is the slow path implementation. Applications should use mutex_trylock().
 */
int mutex_trylock_slow(mutex_t *m);

/**
 * Attempt to acquire a mutex lock without blocking
 *
 * @param m  Pointer to the mutex to try locking
 *
 * @return 0 if the mutex was successfully locked, non-zero if already locked
 *
 * @note This function does not block. It returns immediately whether or not
 *       the lock was acquired.
 */
inline int  __attribute__((always_inline))
mutex_trylock(mutex_t *m)
{
  if(__atomic_always_lock_free(sizeof(intptr_t), 0)) {
    intptr_t expected = 0;
    if(__builtin_expect(__atomic_compare_exchange_n(&m->lock, &expected, 1, 1,
                                                    __ATOMIC_SEQ_CST,
                                                    __ATOMIC_RELAXED), 1))
      return 0;
  }
  return mutex_trylock_slow(m);
}


/**
 * Release a mutex lock (slow path)
 *
 * @param m  Pointer to the mutex to unlock
 *
 * @note This is the slow path implementation called by mutex_unlock() when
 *       there are waiting threads. Applications should use mutex_unlock() instead.
 */
void mutex_unlock_slow(mutex_t *m);

/**
 * Release a mutex lock
 *
 * @param m  Pointer to the mutex to unlock
 *
 * @note The mutex must be currently held by the calling thread. If there are
 *       threads waiting on the mutex, one will be woken up.
 */
inline void  __attribute__((always_inline))
mutex_unlock(mutex_t *m)
{
  if(__atomic_always_lock_free(sizeof(intptr_t), 0)) {
    intptr_t expected = 1;
    if(__builtin_expect(__atomic_compare_exchange_n(&m->lock, &expected, 0, 1,
                                                    __ATOMIC_SEQ_CST,
                                                    __ATOMIC_RELAXED), 1))
      return;
  }
  mutex_unlock_slow(m);
}


#ifdef ENABLE_TASK_WCHAN
#define COND_INITIALIZER(n) {.name = (n)}
#else
#define COND_INITIALIZER(n) {}
#endif

/**
 * Initialize a condition variable
 *
 * @param c     Pointer to the condition variable to initialize
 * @param name  Name of the condition variable for debugging (only used if ENABLE_TASK_WCHAN is defined)
 */
inline void  __attribute__((always_inline))
cond_init(cond_t *c, const char *name)
{
  LIST_INIT(&c->list);
#ifdef ENABLE_TASK_WCHAN
  c->name = name;
#endif
}

/**
 * Signal one thread waiting on a condition variable
 *
 * @param c  Pointer to the condition variable
 *
 * @note Wakes up one thread waiting on the condition variable (if any).
 */
void cond_signal(cond_t *c);

/**
 * Wake all threads waiting on a condition variable
 *
 * @param c  Pointer to the condition variable
 *
 * @note Wakes up all threads currently waiting on the condition variable.
 */
void cond_broadcast(cond_t *c);

/**
 * Wait on a condition variable
 *
 * @param c  Pointer to the condition variable to wait on
 * @param m  Pointer to the mutex that must be held when calling this function
 *
 * @note The mutex is atomically released when the thread goes to sleep, and
 *       reacquired before this function returns. The mutex must be locked
 *       by the calling thread before calling this function.
 */
void cond_wait(cond_t *c, mutex_t *m);

/**
 * Wait on a condition variable with a timeout
 *
 * @param c         Pointer to the condition variable to wait on
 * @param m         Pointer to the mutex that must be held when calling
 * @param deadline  Absolute deadline timestamp (in microseconds)
 *
 * @return 1 if the deadline expired (timeout), 0 if signaled
 *
 * @note The mutex is atomically released when the thread goes to sleep, and
 *       reacquired before this function returns. The mutex must be locked
 *       by the calling thread before calling this function.
 */
int cond_wait_timeout(cond_t *c, mutex_t *m, uint64_t deadline)
  __attribute__((warn_unused_result));

/**
 * Create a thread for CLI/shell activities
 *
 * @param entry   Thread entry point function
 * @param arg     Argument to pass to the entry function
 * @param name    Name of the thread (max 10 characters, will be truncated)
 * @param log_st  Stream to use for logging output
 *
 * @return ERR_OK on success, or an error code on failure
 *
 * @note This is a helper function that creates a thread with appropriate
 *       settings for interactive shell/CLI operations.
 */
error_t thread_create_shell(void *(*entry)(void *arg), void *arg,
                            const char *name, struct stream *log_st);


/**
 * Wait multiplexer for waiting on multiple events
 *
 * Allows a thread to wait on multiple waitable objects simultaneously,
 * similar to select() or poll() for file descriptors.
 */
typedef struct waitmux {
  task_waitable_t wm_waitable;  /**< Waitable for the mux itself */
  int wm_which;                 /**< Index of which event triggered */
  task_t wm_tasks[0];           /**< Flexible array of task structures */
} waitmux_t;



/*
 * SoftIRQs can be raised from any context (even > IRQ_LEVEL_SCHED)
 * as opposed to task_run() which can only be called from IRQ_LEVEL_SCHED
 * and below
 */


#ifndef NUM_SOFTIRQ
#define NUM_SOFTIRQ 0
#endif

#if NUM_SOFTIRQ > 0

/**
 * Raise (trigger) a software interrupt
 *
 * @param id  Software interrupt ID to raise
 *
 * @note SoftIRQs can be raised from any context, including interrupt levels
 *       above IRQ_LEVEL_SCHED, unlike task_run() which requires IRQ_LEVEL_SCHED
 *       or below.
 */
void softirq_raise(uint32_t id);

/**
 * Allocate a new software interrupt handler
 *
 * @param fn   Function to call when the software interrupt is raised
 * @param arg  Argument to pass to the handler function
 *
 * @return The allocated software interrupt ID
 *
 * @note The returned ID can be used with softirq_raise() to trigger the handler.
 */
uint32_t softirq_alloc(void (*fn)(void *arg), void *arg);

#endif
