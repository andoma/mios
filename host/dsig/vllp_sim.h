/*
 * Sim shim: lets the real host VLLP client (vllp.c) compile and run
 * inside freestanding host-mios as a single-threaded, virtual-time
 * simulation peer. Included only when VLLP_SIM is defined; the normal
 * pthread build never sees any of this, so production behaviour is
 * unchanged.
 *
 * It provides, all file-local to vllp.c:
 *   - renamed public symbols (hvllp_*) so they don't clash with the
 *     guest server's vllp_* in the same binary
 *   - no-op pthread mutex/cond/thread (single cooperative thread)
 *   - a private allocator (sim threads must not call mios malloc)
 *   - seeded getrandom, ffs, syslog levels
 *   - the sim reactor entry points (implemented in vllp.c)
 */
#pragma once
#ifdef VLLP_SIM

#include <stdint.h>
#include <stddef.h>
#include <mios/mios.h>   // panic
#include <unistd.h>      // clock_get

// ---- Rename the public API so it does not collide with the guest
//      server (src/net/vllp.c also defines vllp_input, etc.) ----
#define vllp_create_client     hvllp_create_client
#define vllp_create_server     hvllp_create_server
#define vllp_start             hvllp_start
#define vllp_input             hvllp_input
#define vllp_destroy           hvllp_destroy
#define vllp_channel_create    hvllp_channel_create
#define vllp_channel_start     hvllp_channel_start
#define vllp_channel_send      hvllp_channel_send
#define vllp_channel_read      hvllp_channel_read
#define vllp_channel_close     hvllp_channel_close
#define vllp_channel_tx_pending hvllp_channel_tx_pending
#define vllp_is_connected      hvllp_is_connected
#define vllp_channel_id        hvllp_channel_id
#define vllp_strerror          hvllp_strerror
#define vllp_crc32             hvllp_crc32
#define vllp_logf              hvllp_logf

// ---- syslog levels ----
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

// ---- pthreads: single cooperative thread, so these are no-ops.
//      cond wait/timedwait are only reached in code paths vllp.c
//      replaces with the sim pump, so they may safely be no-ops too. ----
typedef int vsim_mutex_t;
typedef int vsim_cond_t;
typedef int vsim_condattr_t;
typedef long vsim_thread_t;

#define pthread_mutex_t        vsim_mutex_t
#define pthread_cond_t         vsim_cond_t
#define pthread_condattr_t     vsim_condattr_t
#define pthread_t              vsim_thread_t

#define pthread_mutex_init(m, a)      ((void)0)
#define pthread_mutex_destroy(m)      ((void)0)
#define pthread_mutex_lock(m)         ((void)0)
#define pthread_mutex_unlock(m)       ((void)0)
#define pthread_cond_init(c, a)       ((void)0)
#define pthread_cond_destroy(c)       ((void)0)
#define pthread_cond_signal(c)        ((void)0)
#define pthread_cond_broadcast(c)     ((void)0)
#define pthread_cond_wait(c, m)       ((void)0)
#define pthread_cond_timedwait(c, m, t) (0)
#define pthread_condattr_init(a)      ((void)0)
#define pthread_condattr_setclock(a, c) ((void)0)
#define pthread_create(t, a, fn, arg) (0)
#define pthread_join(t, r)            (0)
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME  0

// ---- getrandom: deterministic, seeded per client ----
#define GRND_INSECURE 0
uint32_t vsim_rand32(void);
static inline long
vsim_getrandom(void *buf, size_t n, unsigned flags)
{
  (void)flags;
  uint8_t *p = buf;
  for(size_t i = 0; i < n; i++)
    p[i] = vsim_rand32();
  return n;
}
#define getrandom vsim_getrandom

#define ffs __builtin_ffs
#define strerror(e) ""

// ---- private allocator (mios malloc uses a task mutex, unusable from
//      a sim thread) ----
void *vsim_malloc(size_t size);
void *vsim_calloc(size_t nmemb, size_t size);
void *vsim_realloc(void *ptr, size_t size);
void  vsim_free(void *ptr);
#define malloc  vsim_malloc
#define calloc  vsim_calloc
#define realloc vsim_realloc
#define free    vsim_free

#define abort() panic("hvllp: abort")
#define alloca __builtin_alloca
char *vsim_strdup(const char *src);
#define strdup vsim_strdup

// ---- sim reactor entry points (implemented in vllp.c) ----
struct vllp;

// recv one DSIG frame from the transport, or -1 at the deadline. Writes
// the signal id to *id.
typedef long (*vsim_recv_fn)(void *tr, uint32_t *id, void *buf, size_t buflen,
                             int64_t deadline);

// Make the client a sim peer: seed its RNG and wire the transport it
// pulls inbound frames from. rxid is the signal it accepts.
void hvllp_sim_setup(struct vllp *v, uint64_t seed, void *tr,
                     vsim_recv_fn recv, uint32_t rxid);

// Run the client's protocol loop in virtual time until 'deadline'.
void hvllp_sim_run(struct vllp *v, int64_t deadline);

// One cooperative pump step, bounded by deadline.
void hvllp_sim_poll(struct vllp *v, int64_t deadline);

// Free a buffer returned by hvllp_channel_read.
void hvllp_sim_free(void *p);

#endif // VLLP_SIM
