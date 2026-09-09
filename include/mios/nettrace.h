#pragma once

#include <stdint.h>
#include <stddef.h>

// One-off packet-path tracing: record (identity, timestamp) for frames on
// up to two dsig signal ids as they pass fixed points, so one packet can
// be followed across hops and a delay attributed to a stage instead of
// guessed at.
//
// Identity is a CRC32 of the payload. Nothing on the wire carries a
// sequence number that survives every layer, but the bytes themselves are
// the same at every hop. It is deliberately not unique: a stop-and-wait
// retransmission is byte-identical to what it repeats, so repeats show up
// as repeats, which is information rather than a defect -- just do not
// read a dump assuming one key means one packet.
//
// Boards do not share a clock. Cross-board deltas therefore need an
// anchor: NETTRACE_MARK carries a caller-chosen key (a TDMA frame
// counter, say) that both ends log against the same event, which pins
// their offset. Within one board the timestamps are directly comparable
// and no anchor is needed.
//
// Records the first NETTRACE_ENTRIES events and then stops, rather than
// wrapping: the interesting thing is a burst from a known start, and a
// ring would leave a tail whose beginning has already scrolled away.

#define NETTRACE_ENTRIES 256

// Stages recorded by the dsig core.
#define NETTRACE_RX    0   // arrived from a netif
#define NETTRACE_TX    1   // handed to the netifs for output
#define NETTRACE_MARK  2   // clock anchor; key is the caller's
#define NETTRACE_APP   8   // first tag free for application stages

typedef struct {
  uint32_t nt_key;   // crc32 of the payload, or a mark's key
  uint32_t nt_ts;    // clock_get() microseconds, low 32 bits
  uint16_t nt_id;
  uint8_t nt_tag;
  uint8_t nt_len;
} nettrace_ent_t;

// Applications naming their own stages override this; the core prints
// the number when it returns NULL.
const char *nettrace_tag_name(uint8_t tag);

#ifdef ENABLE_NETTRACE

void nettrace_pkt(uint8_t tag, uint32_t id, const void *data, size_t len);
void nettrace_mark(uint8_t tag, uint32_t key);

#else

static inline void nettrace_pkt(uint8_t tag, uint32_t id,
                                const void *data, size_t len)
{
  (void)tag; (void)id; (void)data; (void)len;
}

static inline void nettrace_mark(uint8_t tag, uint32_t key)
{
  (void)tag; (void)key;
}

#endif
