#pragma once

// #define PBUF_ORIGIN_TRACE

#include <sys/queue.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ENABLE_PBUF_DYNAMIC_SIZE

// Buffer size chosen once at startup instead of compiled in, so a single
// binary can exercise the stack at the sizes different targets actually
// use -- a path that is fine against a roomy pool can overrun a tight
// one, and that difference is invisible if every build uses the same
// constant.
//
// Deliberately opt-in and NOT the default: constrained targets want the
// compile-time constant, both for the code it generates on a hot path and
// for the _Static_assert()s that check a driver's minimum against it
// (see src/drivers/rtl8168.c). Enable per platform, never globally.
int pbuf_data_size(void);

// Must be called before the first pool is created; panics otherwise,
// since buffers already carved at the old size would be mis-sized.
void pbuf_set_data_size(int size);

// How many buffers the default pool gets, overriding PBUF_DEFAULT_COUNT.
// Same ordering rule as above. A deliberately small pool is how you
// exercise what the stack does when it runs out, which is a different
// code path from the one a roomy pool ever reaches -- non-blocking
// callers start getting NULL and blocking ones sleep.
void pbuf_set_pool_count(int count);

#define PBUF_DATA_SIZE pbuf_data_size()

#else

#ifndef PBUF_DATA_SIZE
#define PBUF_DATA_SIZE 512
#endif

#endif

#ifdef PBUF_ORIGIN_TRACE
#define PBUF_ORIGIN_ARG_DECL , const char *origin
#define PBUF_ORIGIN_ARG_CALL , origin
#else
#define PBUF_ORIGIN_ARG_DECL
#define PBUF_ORIGIN_ARG_CALL
#endif


STAILQ_HEAD(pbuf_queue, pbuf);

#define PBUF_SOP       0x1
#define PBUF_EOP       0x2
#define PBUF_CKSUM_OK  0x4
#define PBUF_SEQ       0x8
#define PBUF_BCAST     0x10
#define PBUF_MCAST     0x20
#define PBUF_TIMESTAMP 0x40 // Packet data starts with pbuf_timestamp

typedef struct pbuf {

  STAILQ_ENTRY(pbuf) pb_link;
#define pb_next pb_link.stqe_next

  uint8_t pb_flags;
  uint8_t pb_credits;
  uint16_t pb_pktlen;
  uint16_t pb_offset;
  uint16_t pb_buflen;

  void *pb_data;

} pbuf_t;

struct netif;
struct pbuf_timestamp;

typedef void (pbuf_tx_cb_t)(struct netif *ni,
                            const struct pbuf_timestamp *pt);

typedef struct pbuf_timestamp {

  pbuf_tx_cb_t *pt_cb;
  uint32_t pt_id;

  uint32_t pt_seconds;
  int32_t pt_nanoseconds;

} pbuf_timestamp_t;



void pbuf_reset(pbuf_t *pb, size_t header_size, size_t len);

static inline void *pbuf_data(pbuf_t *pb, size_t offset) {
  return pb->pb_data + pb->pb_offset + offset;
}

static inline const void *pbuf_cdata(const pbuf_t *pb, size_t offset) {
  return pb->pb_data + pb->pb_offset + offset;
}

// Remove from head
__attribute__((warn_unused_result))
pbuf_t *pbuf_drop(pbuf_t *pb, size_t bytes, int free_when_empty);

// Remove from tail
void pbuf_trim(pbuf_t *pb, size_t bytes);

__attribute__((warn_unused_result))
pbuf_t *pbuf_prepend(pbuf_t *pb, size_t bytes, int wait, size_t extra_offset);

// returns number of bytes missing (0 == ok)
__attribute__((warn_unused_result))
size_t pbuf_pullup(pbuf_t *pb, size_t bytes);

void pbuf_free(pbuf_t *pb);

__attribute__((warn_unused_result))
pbuf_t *pbuf_make0(int offset, int wait PBUF_ORIGIN_ARG_DECL);

__attribute__((warn_unused_result))
pbuf_t *pbuf_copy0(const pbuf_t *src, int wait PBUF_ORIGIN_ARG_DECL);

__attribute__((warn_unused_result))
pbuf_t *pbuf_copy_pkt(const pbuf_t *src, int wait);

__attribute__((warn_unused_result))
void *pbuf_append(pbuf_t *pb, size_t bytes);

__attribute__((warn_unused_result))
pbuf_t *pbuf_splice(struct pbuf_queue *pq);

__attribute__((warn_unused_result))
pbuf_t *pbuf_read(pbuf_t *pb, void *ptr, size_t len);

struct pushpull;
__attribute__((warn_unused_result))
pbuf_t *pbuf_write(pbuf_t *pb, const void *ptr, size_t len,
                   const struct pushpull *p, int wait);

__attribute__((warn_unused_result))
int pbuf_read_at(pbuf_t *pb, void *out, size_t offset, size_t len);

__attribute__((warn_unused_result))
int pbuf_memcmp_at(pbuf_t *pb, const void *data, size_t offset, size_t len);

int pbuf_buffer_avail(void);

// Non-blocking allocations that failed for want of a buffer, since boot.
// Nonzero means the pool ran dry at least once. Worth checking from a
// stress test rather than assuming it got the pressure it asked for: a
// pool that turns out to be big enough makes such a test vacuous.
unsigned int pbuf_alloc_fail_count(void);

#ifdef ENABLE_PBUF_FAULT_INJECT

// Fail this percentage of non-blocking buffer allocations, on purpose.
//
// Simply shrinking the pool does not reliably produce exhaustion: a
// request/response test holds only a couple of buffers at a time, so it
// passes with a tiny pool having never once run dry. Forcing the failure
// reaches the out-of-buffers branches directly, and at a rate you choose
// rather than one you hope for.
//
// Deterministic: its own seeded generator, so a failing run reproduces
// and it does not perturb whatever else is drawing random numbers. pct 0
// disables. Only the non-blocking path is affected -- making the
// blocking path fail would deadlock callers that are entitled to wait.
void pbuf_fault_inject(unsigned int pct, uint32_t seed);

#endif

int pbuf_buffer_total(void);


// =========================================================
// Debug helpers
// =========================================================

struct stream;
void pbuf_status(struct stream *st);

void pbuf_dump(const char *prefix, const pbuf_t *pb, int full);

struct stream;
void pbuf_dump_stream(const char *prefix, const pbuf_t *pb, int full,
                      struct stream *st);

// =========================================================
// All functions below here assume irq_forbid(IRQ_LEVEL_NET)
// =========================================================

void pbuf_data_add(void *start, void *end);

__attribute__((warn_unused_result, malloc))
void *pbuf_data_get0(int wait PBUF_ORIGIN_ARG_DECL);

void pbuf_data_put(void *ptr);

void pbuf_alloc(size_t count);

__attribute__((warn_unused_result,malloc))
pbuf_t *pbuf_get0(int wait PBUF_ORIGIN_ARG_DECL);

void pbuf_put(pbuf_t *pb);

void pbuf_free_irq_blocked(pbuf_t *pb);

__attribute__((warn_unused_result))
pbuf_t *pbuf_make_irq_blocked0(int offset, int wait PBUF_ORIGIN_ARG_DECL);

void pbuf_free_queue_irq_blocked(struct pbuf_queue *pq);


#ifdef PBUF_ORIGIN_TRACE
#define pbuf_data_get(wait) pbuf_data_get0(wait, __FUNCTION__)
#define pbuf_get(wait) pbuf_get0(wait, __FUNCTION__)
#define pbuf_copy(src, wait) pbuf_copy0(src, wait, __FUNCTION__)
#define pbuf_make(offset, wait) pbuf_make0(offset, wait, __FUNCTION__)
#define pbuf_make_irq_blocked(offset, wait) pbuf_make_irq_blocked0(offset, wait, __FUNCTION__)
#else
#define pbuf_data_get(wait) pbuf_data_get0(wait)
#define pbuf_get(wait) pbuf_get0(wait)
#define pbuf_copy(src, wait) pbuf_copy0(src, wait)
#define pbuf_make(offset, wait) pbuf_make0(offset, wait)
#define pbuf_make_irq_blocked(offset, wait) pbuf_make_irq_blocked0(offset, wait)
#endif
