/*
 * vllp-frames: the mios VLLP server against a peer that sends things no
 * well-behaved client would.
 *
 * Every other VLLP suite drives the server with a real client, so it only
 * ever reaches the paths a correct implementation asks for. The server is
 * on a bus, though, and a bus carries whatever anyone puts on it: a peer
 * mid-firmware-update, a peer that has lost sync, or simply noise. This
 * suite hand-assembles frames so those paths get exercised on purpose.
 *
 * The peer here is deliberately not a VLLP implementation -- it is a few
 * dozen lines that know the wire format from docs/vllp.txt. It mirrors the
 * server's own SE bookkeeping (that part has to be right or the server
 * rejects everything as out of sequence) and nothing else.
 *
 * Each phase ends by checking that the server still works and that the
 * buffer pool is where it started, because the interesting failures here
 * are not "wrong answer" but "crashed" and "quietly ate the pool".
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/vllp.h>

#include "net/pbuf.h"
#include "util/crc32.h"

#include "hosttest.h"
#include "sim.h"
#include "vcan.h"

#define SEC 1000000ull

/* The server, from the peer's point of view. */
#define SRV_TX 0x300
#define SRV_RX 0x301
#define MTU    64

/* Wire constants, from docs/vllp.txt. Private to vllp.c, so restated. */
#define F_SYN 0x0f
#define F_S   0x80
#define F_E   0x40
#define F_F   0x20
#define F_L   0x10

#define CMC_CHANNEL           14
#define CMC_OP_OPEN           0
#define CMC_OP_OPEN_RESPONSE  2
#define CMC_OP_UNUSED         1   /* reserved by the spec; nothing sends it */

static int fails;

#define FCHECK(cond, ...)                                        \
  do { if(!(cond)) { fails++;                                    \
       hosttest_check(0, __FILE__, __LINE__, __VA_ARGS__); } } while(0)


typedef struct peer {
  vcan_t *vcan;
  uint32_t cookie;
  uint8_t se;             /* mirrors the server's own S/E bookkeeping */
  uint32_t iv_cnt;        /* per-session channel IV counter */
  uint32_t cmc_tx_iv;     /* IV for messages we send on the CMC */
  uint32_t cmc_rx_iv;     /* ...and expect on */
  volatile int done;
  volatile int failed_setup;
} peer_t;


/* ---- wire helpers ---- */

static int
pad_ladder(int len)
{
  if(len < 12) return 12;
  if(len < 16) return 16;
  if(len < 20) return 20;
  if(len < 24) return 24;
  if(len < 32) return 32;
  if(len < 48) return 48;
  return 64;
}


/* Send one frame, applying the FDCAN length adaptation the server
   expects: anything over 8 bytes is padded up the DLC ladder with the pad
   count in the final byte. */
static void
tx(peer_t *p, const void *data, size_t len)
{
  uint8_t f[72];
  memcpy(f, data, len);
  if(len > 8) {
    const int total = pad_ladder(len);
    const int pad = total - len;
    memset(f + len, 0, pad);
    f[total - 1] = pad;
    len = total;
  }
  vcan_peer_send(p->vcan, SRV_RX, f, len);
}


/* Receive one frame from the server, stripping the pad. -1 on timeout. */
static long
rx(peer_t *p, uint8_t *buf, size_t buflen, uint64_t deadline)
{
  while(1) {
    uint32_t id;
    long n = vcan_peer_recv(p->vcan, &id, buf, buflen, deadline);
    if(n < 0)
      return -1;
    if(id != SRV_TX)
      continue;
    if(n > 8) {
      const int pad = buf[n - 1];
      if(pad < n)
        n -= pad;
    }
    return n;
  }
}


/* Drain whatever the server has to say, keeping our SE in step. Returns
   the number of data frames seen (ACKs do not count). */
static int
drain(peer_t *p, uint64_t for_us)
{
  const uint64_t deadline = clock_get() + for_us;
  uint8_t f[72];
  int data = 0;

  while(1) {
    long n = rx(p, f, sizeof(f), deadline);
    if(n < 1)
      return data;
    if((f[0] & 0x1f) == 0x1f)
      continue;                 /* pure ACK: consumes no sequence */
    /* A data frame. Accept it and flip what we expect next, exactly as
       the server does when it accepts one of ours. */
    p->se ^= F_E;
    data++;
  }
}


/* Open a session. Returns 0 on success. */
static int
handshake(peer_t *p, uint32_t cookie)
{
  p->cookie = cookie;
  p->se = F_E;

  uint8_t syn[7];
  syn[0] = F_SYN;
  syn[1] = 2;                   /* version */
  syn[2] = MTU - 1;             /* the server's adapted MTU */
  memcpy(syn + 3, &p->cookie, 4);
  tx(p, syn, sizeof(syn));

  uint8_t f[72];
  const uint64_t deadline = clock_get() + 3 * SEC;
  while(1) {
    long n = rx(p, f, sizeof(f), deadline);
    if(n < 0)
      return -1;
    if(n == 7 && f[0] == (F_E | 0x1f))
      break;                    /* the ACK that answers our SYN */
  }

  /* Per-session channel IVs. The server derives the management channel's
     from the cookie and its own counter, and mirrors the polarity: what
     it transmits with is what we receive with. Keep the counter in step
     -- the server advances it once per channel it is asked to open,
     including ones it refuses. */
  p->iv_cnt = 1;
  const uint32_t cmc_iv = crc32(p->cookie, &p->iv_cnt, sizeof(p->iv_cnt));
  p->cmc_rx_iv = cmc_iv;
  p->cmc_tx_iv = ~cmc_iv;
  return 0;
}


/* Send one fragment on a channel. `last` sets the end-of-message bit. */
static void
tx_fragment(peer_t *p, int channel, const void *payload, size_t len,
            int last)
{
  uint8_t f[72];
  p->se ^= F_S;                 /* a data frame consumes our sequence */
  f[0] = p->se | (last ? F_L : 0) | F_F | channel;
  memcpy(f + 1, payload, len);
  tx(p, f, len + 1);
}


/* Send a complete message on the management channel: fragment it, append
   the message CRC to the last fragment, and keep the IV rolling. */
static void
tx_cmc_message(peer_t *p, const void *msg, size_t len)
{
  uint8_t buf[256];
  memcpy(buf, msg, len);

  const uint32_t crc = ~crc32(p->cmc_tx_iv, buf, len);
  buf[len + 0] = crc;
  buf[len + 1] = crc >> 8;
  buf[len + 2] = crc >> 16;
  buf[len + 3] = crc >> 24;
  len += 4;
  p->cmc_tx_iv++;

  const size_t frag = MTU - 2;  /* header, and one byte for the pad count */
  size_t off = 0;
  while(off < len) {
    const size_t n = MIN(frag, len - off);
    tx_fragment(p, CMC_CHANNEL, buf + off, n, off + n == len);
    off += n;
    drain(p, 200000);           /* let the server ack and advance */
  }
}


/* Ask the server to open `service`, and report the error code it answers
   with. Returns the 16-bit code, or -1 if nothing came back. */
static int
open_channel(peer_t *p, int channel, const char *service)
{
  uint8_t msg[64];
  const size_t namelen = strlen(service);
  msg[0] = (CMC_OP_OPEN << 4) | channel;
  memcpy(msg + 1, service, namelen);

  /* The server generates a channel IV per OPEN it receives. */
  p->iv_cnt++;

  tx_cmc_message(p, msg, 1 + namelen);

  /* The answer arrives as a management-channel message. We only need the
     opcode and the code, and every CMC response fits one fragment. */
  uint8_t f[72];
  const uint64_t deadline = clock_get() + 5 * SEC;
  while(1) {
    long n = rx(p, f, sizeof(f), deadline);
    if(n < 0)
      return -1;
    if((f[0] & 0x1f) == 0x1f)
      continue;
    if((f[0] & 0xf) != CMC_CHANNEL)
      continue;
    p->se ^= F_E;
    /* [hdr][opcode|channel][err lo][err hi][crc32] */
    if(n >= 4 && (f[1] >> 4) == CMC_OP_OPEN_RESPONSE)
      return f[2] | (f[3] << 8);
  }
}


/* Is the server still able to do its job? Opens a real service on a fresh
   session and checks the answer. */
static int
server_healthy(peer_t *p, uint32_t cookie)
{
  if(handshake(p, cookie))
    return 0;
  return open_channel(p, 0, "echo") == 0;
}


/* ---- phases ---- */

/* An oversized message on the *management* channel.
 *
 * The reassembly limit is enforced by closing the channel, and closing a
 * channel calls into the application bound to it -- but the management
 * channel is the one channel that has no application. A peer that sends
 * more fragments than the limit without ever setting the last-fragment
 * bit therefore used to dereference a NULL function pointer, from the net
 * thread, on a frame anyone on the bus can send.
 */
static void
phase_oversize_cmc(peer_t *p)
{
  hosttest_log("-- oversized management message");

  const int pool_before = pbuf_buffer_avail();

  FCHECK(handshake(p, 0xa1b2c3d4) == 0, "oversize: handshake failed");

  /* Fragments that never end. Enough to exceed the reassembly limit
     whatever the configured buffer size is: the limit is a number of
     buffers, so size the count from the buffer size. */
  uint8_t junk[MTU - 2];
  memset(junk, 0x5a, sizeof(junk));
  const int frags = (PBUF_DATA_SIZE * 6) / sizeof(junk) + 8;

  for(int i = 0; i < frags; i++) {
    tx_fragment(p, CMC_CHANNEL, junk, sizeof(junk), 0);
    drain(p, 100000);
  }

  /* Surviving this at all is most of the point. */
  hosttest_log("   sent %d unterminated fragments, server still alive",
               frags);

  /* The session is expected to be gone -- an oversized management
     message is not recoverable -- but the server must still serve. */
  FCHECK(server_healthy(p, 0xa1b2c3d5),
         "oversize: server no longer opens channels afterwards");

  /* And it must not have eaten the pool on the way. */
  drain(p, SEC);
  const int pool_after = pbuf_buffer_avail();
  FCHECK(pool_after >= pool_before - 2,
         "oversize: %d buffers lost", pool_before - pool_after);
}


/* A management message with an opcode the spec reserves. Nothing sends
   these, so the handler's default case never ran in normal operation --
   and it returned without freeing the message it had been handed. */
static void
phase_unknown_opcode(peer_t *p)
{
  hosttest_log("-- reserved management opcode");

  const int pool_before = pbuf_buffer_avail();

  /* Repeated, because one leaked buffer hides inside any tolerance. */
  const int rounds = 20;
  for(int i = 0; i < rounds; i++) {
    if(handshake(p, 0xb0000000 + i)) {
      FCHECK(0, "unknown_opcode: handshake %d failed", i);
      return;
    }
    uint8_t msg[1] = { (CMC_OP_UNUSED << 4) | 0 };
    tx_cmc_message(p, msg, sizeof(msg));
    drain(p, 500000);
  }

  FCHECK(server_healthy(p, 0xb0ffffff),
         "unknown_opcode: server broken after %d reserved opcodes", rounds);

  drain(p, SEC);
  const int pool_after = pbuf_buffer_avail();
  hosttest_log("   %d reserved opcodes, pool %d -> %d", rounds, pool_before,
               pool_after);
  FCHECK(pool_after >= pool_before - 2,
         "unknown_opcode: %d buffers lost over %d messages -- the handler "
         "is dropping the message without freeing it",
         pool_before - pool_after, rounds);
}


/* A management message left half-reassembled when the session dies.
 *
 * Every other channel is destroyed on disconnect, which frees whatever it
 * was holding. The management channel is not -- it outlives the session --
 * so a partial message stayed on its reassembly queue and the next
 * session's fragments were appended to it. The result is a message that
 * cannot pass its CRC, which resets the link, which strands another
 * partial message: a link that never recovers, and a buffer lost each
 * time round.
 */
static void
phase_partial_across_reset(peer_t *p)
{
  hosttest_log("-- partial management message across a session reset");

  const int pool_before = pbuf_buffer_avail();

  for(int i = 0; i < 3; i++) {
    if(handshake(p, 0xc0000000 + i)) {
      FCHECK(0, "partial: handshake %d failed", i);
      return;
    }

    /* One fragment of a message that never completes... */
    uint8_t junk[16];
    memset(junk, 0x33 + i, sizeof(junk));
    tx_fragment(p, CMC_CHANNEL, junk, sizeof(junk), 0);
    drain(p, 200000);

    /* ...then go quiet for longer than the link timeout so the server
       tears the session down with that fragment still in hand. */
    usleep(5 * SEC);
  }

  /* A fresh session must work. If the stale fragments are still queued,
     this message reassembles as [stale][ours] and fails its CRC. */
  FCHECK(server_healthy(p, 0xc0ffffff),
         "partial: the server cannot open a channel on a new session -- a "
         "half-reassembled management message from a dead session is "
         "still queued and corrupting the new one");

  drain(p, SEC);
  const int pool_after = pbuf_buffer_avail();
  hosttest_log("   pool %d -> %d", pool_before, pool_after);
  FCHECK(pool_after >= pool_before - 2,
         "partial: %d buffers lost", pool_before - pool_after);
}


static void
peer_fn(void *arg)
{
  peer_t *p = arg;

  phase_oversize_cmc(p);
  phase_unknown_opcode(p);
  phase_partial_across_reset(p);

  p->done = 1;
}


static int
pred_done(void *arg)
{
  peer_t *p = arg;
  return p->done;
}


static int
test_vllp_frames(void)
{
  hosttest_log("---- mios VLLP server vs a hand-assembled peer ----");

  vcan_t *vcan = vcan_create("vcan0", MTU);
  vllp_server_create(SRV_TX, SRV_RX, MTU, 3);
  vcan_set_link(vcan, 1);

  peer_t *p = calloc(1, sizeof(peer_t));
  p->vcan = vcan;

  sim_thread_create("frame-peer", peer_fn, p, 1 << 18);

  CHECK(hosttest_wait(pred_done, p, 300 * SEC), "peer did not finish");
  return fails;
}

HOSTTEST_SUITE("vllp-frames", test_vllp_frames, 0);
