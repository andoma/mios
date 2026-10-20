/*
 * evlog: timestamps of the system log as the host sees them, i.e. the
 * mios "log" service (evlog_svc_pull() in src/kernel/eventlog.c) read by
 * the production host decoder (host/dsig/vllp_logstream.c).
 *
 * The log stores each entry's time as a delta from the previous entry
 * and the service sends "milliseconds ago", reconstructed from those
 * deltas at the moment the entry is pulled. Two things used to go wrong
 * and together made BMCU log lines on the Jetson alternate between the
 * right time and ~14.7 days in the past:
 *
 *  - evlog() samples the clock before it takes the log mutex, so two
 *    writers can commit out of order. The late one was given a zero
 *    delta but also moved the head timestamp backwards, so every entry
 *    after it was reconstructed in the future. Pulled at once, its age
 *    was negative and wrapped as an unsigned to a 7 byte "ms ago".
 *
 *  - The host decoder shifted the delta bytes as int, so bytes 4 and
 *    up folded back into the low bits (shift counts wrap mod 32 on
 *    aarch64 and x86). The wrapped "-1 ms" decoded as 1271377919 ms.
 *
 * "evlog-decode" runs the decoder on hand-made frames. "evlog" provokes
 * the writer race on the device and checks the reconstructed time of
 * each entry over a real VLLP link, with the host stack as the client.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <mios/vllp.h>
#include <mios/task.h>
#include <mios/stream.h>
#include <mios/eventlog.h>

#include "hosttest.h"
#include "sim.h"
#include "vcan.h"
#include "../../../host/dsig/vllp_sim_api.h"
#include "../../../host/dsig/vllp_logstream.h"

#define SEC 1000000ull
#define MS 1000ull

#define DEV_TXID 0x200
#define DEV_RXID 0x201
#define MTU 64


// ---- evlog-decode: the host decoder on its own

static int
decode_ms_ago(int64_t ms_ago)
{
  uint8_t frame[16];
  int tslen = 0;
  for(uint64_t v = ms_ago; v; v >>= 8)
    frame[1 + tslen++] = v;
  frame[0] = LOG_INFO | (tslen << 3);
  memcpy(frame + 1 + tslen, "hello", 5);

  uint32_t seq = 41;
  int level;
  int64_t got;
  const char *msg;
  size_t msglen;

  if(!CHECK(vllp_logstream_decode(frame, 1 + tslen + 5, &seq, &level, &got,
                                  &msg, &msglen) == 0,
            "%d byte delta: decode failed", tslen))
    return 1;

  int fails = 0;
  fails += !CHECK(got == ms_ago, "%d byte delta: decoded %lld ms, sent %lld",
                  tslen, (long long)got, (long long)ms_ago);
  fails += !CHECK(level == LOG_INFO, "%d byte delta: level %d", tslen, level);
  fails += !CHECK(seq == 42, "%d byte delta: seq %u", tslen, seq);
  fails += !CHECK(msglen == 5 && !memcmp(msg, "hello", 5),
                  "%d byte delta: message mangled", tslen);
  return fails;
}

static int
test_evlog_decode(void)
{
  int fails = 0;

  // Every width the 3 bit size field can say, with the top byte both
  // below and above 0x80 to catch sign extension
  for(int bytes = 1; bytes <= 7; bytes++) {
    const int64_t low = 1LL << ((bytes - 1) * 8);
    fails += decode_ms_ago(low | 0x5a);
    fails += decode_ms_ago((low << 8) - 1);
  }
  // Older than 49.7 days, the first age that needs 5 bytes
  fails += decode_ms_ago(60LL * 86400 * 1000);

  // Discontinuity: the sequence number comes from the frame
  const uint8_t disc[] = { 0x40 | (1 << 3) | LOG_ERR,
                           0x10, 0x20, 0x30, 0x40, 7, 'x' };
  uint32_t seq = 0;
  int level;
  int64_t ms_ago;
  const char *msg;
  size_t msglen;
  fails += !CHECK(vllp_logstream_decode(disc, sizeof(disc), &seq, &level,
                                        &ms_ago, &msg, &msglen) == 0,
                  "discontinuity: decode failed");
  fails += !CHECK(seq == 0x40302011, "discontinuity: seq %x", seq);
  fails += !CHECK(level == LOG_ERR && ms_ago == 7 &&
                  msglen == 1 && msg[0] == 'x',
                  "discontinuity: fields wrong");

  // Truncated frames are rejected, not read past the end
  const uint8_t trunc[] = { 0x40 | (3 << 3), 1, 2, 3, 4, 5, 6 };
  for(size_t len = 0; len < sizeof(trunc); len++) {
    fails += !CHECK(vllp_logstream_decode(trunc, len, &seq, &level,
                                          &ms_ago, &msg, &msglen) == -1,
                    "%zu byte truncated frame accepted", len);
  }
  return fails;
}

HOSTTEST_SUITE("evlog-decode", test_evlog_decode, 0);


// ---- evlog: the device side over a real link

#define NUM_ENTRIES 3

static const char *entry_names[NUM_ENTRIES] = {
  "evlog-holder", "evlog-racer", "evlog-probe"
};

typedef struct scenario {
  vcan_t *vcan;
  hvllp_t *v;

  volatile int ready;   // client has the log channel open and drained
  volatile int done;
  volatile int logged;  // device has written all entries

  // Filled in by the client: when each entry was received and how many
  // ms ago the device said it was logged. -1 = not seen
  int64_t recv_at[NUM_ENTRIES];
  int64_t ms_ago[NUM_ENTRIES];
  int bad_frames;
  int failures;
} scenario_t;

#define SCHECK(sc, cond, ...) \
  do { if(!(cond)) { (sc)->failures++; \
       hosttest_check(0, __FILE__, __LINE__, __VA_ARGS__); } } while(0)

static void
client_tx(void *opaque, const void *data, size_t len)
{
  scenario_t *sc = opaque;
  vcan_peer_send(sc->vcan, DEV_RXID, data, len);
}

static long
client_recv(void *tr, uint32_t *id, void *buf, size_t buflen, int64_t deadline)
{
  scenario_t *sc = tr;
  while(1) {
    long n = vcan_peer_recv(sc->vcan, id, buf, buflen, (uint64_t)deadline);
    if(n < 0)
      return -1;
    if(*id == DEV_TXID)
      return n;
  }
}

static void
client_log(void *opaque, int level, const char *msg)
{
  if(level <= 4 /* LOG_WARNING */)
    hosttest_log("  client WARN: %s", msg);
}

static int
all_seen(const scenario_t *sc)
{
  for(int i = 0; i < NUM_ENTRIES; i++)
    if(sc->recv_at[i] == -1)
      return 0;
  return 1;
}

static void
scenario_fn(void *arg)
{
  scenario_t *sc = arg;

  hvllp_t *v = hvllp_create_client(MTU, 3, HVLLP_FDCAN_ADAPTATION, sc,
                                   client_tx, client_log);
  sc->v = v;
  hvllp_sim_setup(v, 0x1234, sc, client_recv, DEV_TXID);
  hvllp_start(v);

  int64_t dl = clock_get() + 3 * SEC;
  while(!hvllp_is_connected(v) && clock_get() < dl)
    hvllp_sim_poll(v, dl);
  SCHECK(sc, hvllp_is_connected(v), "did not connect");

  hvllp_channel_t *ch = hvllp_channel_create(v, "log", 0, NULL, NULL,
                                             NULL, NULL);
  SCHECK(sc, ch != NULL, "log: open failed");

  // Let the backlog drain before the device starts logging, so our
  // entries are pulled the moment they are written
  hvllp_sim_run(v, clock_get() + 100 * MS);
  sc->ready = 1;

  uint32_t seq = 0;
  while(ch != NULL && !(sc->logged && all_seen(sc))) {
    void *data = NULL;
    size_t len = 0;
    if(hvllp_channel_read(ch, &data, &len, 3 * SEC) || data == NULL) {
      SCHECK(sc, 0, "log: read failed before all entries arrived");
      break;
    }
    const int64_t now = clock_get();

    int level;
    int64_t ms_ago;
    const char *msg;
    size_t msglen;
    if(vllp_logstream_decode(data, len, &seq, &level, &ms_ago,
                             &msg, &msglen)) {
      sc->bad_frames++;
    } else {
      for(int i = 0; i < NUM_ENTRIES; i++) {
        if(msglen == strlen(entry_names[i]) &&
           !memcmp(msg, entry_names[i], msglen)) {
          sc->recv_at[i] = now;
          sc->ms_ago[i] = ms_ago;
        }
      }
    }
    hvllp_sim_free(data);
  }

  hvllp_channel_close(ch, 0, 0);
  hvllp_destroy(v);
  sc->done = 1;
}

static int
pred_ready(void *arg)
{
  scenario_t *sc = arg;
  return sc->ready;
}

static int
pred_done(void *arg)
{
  scenario_t *sc = arg;
  return sc->done;
}

static void *
racer(void *arg)
{
  evlog(LOG_INFO, "evlog-racer");
  return NULL;
}

static int
test_evlog(void)
{
  vcan_t *vcan = vcan_create("vcan0", MTU);
  vllp_server_create(DEV_TXID, DEV_RXID, MTU, 3);
  vcan_set_link(vcan, 1);

  scenario_t *sc = calloc(1, sizeof(scenario_t));
  sc->vcan = vcan;
  for(int i = 0; i < NUM_ENTRIES; i++)
    sc->recv_at[i] = -1;

  sim_thread_create("evlog-client", scenario_fn, sc, 1 << 20);

  if(!CHECK(hosttest_wait(pred_ready, sc, 10 * SEC), "client never ready"))
    return 1;

  // Hold the log mutex as an entry in progress. The racer preempts us,
  // samples the clock at t0 and blocks on the mutex. We commit at t0 +
  // 10 ms and the racer, woken at once, commits after us with t0
  struct stream *st = evlog_stream_begin();
  const int64_t t0 = clock_get();
  thread_create(racer, NULL, 0, "racer", 0, 5);
  usleep(10 * MS);
  stream_write(st, entry_names[0], strlen(entry_names[0]), 0);
  evlog_stream_end(LOG_INFO);
  const int64_t t_holder = clock_get();

  usleep(5 * MS);
  evlog(LOG_INFO, "evlog-probe");
  const int64_t t_probe = clock_get();
  sc->logged = 1;

  hosttest_log("  t0=%lld holder=+%lld probe=+%lld us", (long long)t0,
               (long long)(t_holder - t0), (long long)(t_probe - t0));

  CHECK(hosttest_wait(pred_done, sc, 30 * SEC), "scenario did not finish");

  // When each entry should read as logged. The racer's own clock said
  // t0, but it is ordered after the holder so its time is pinned there
  const int64_t expected[NUM_ENTRIES] = { t_holder, t_holder, t_probe };

  int fails = sc->failures;
  fails += !CHECK(sc->bad_frames == 0, "%d undecodable frames",
                  sc->bad_frames);

  for(int i = 0; i < NUM_ENTRIES; i++) {
    if(!CHECK(sc->recv_at[i] != -1, "%s never received", entry_names[i])) {
      fails++;
      continue;
    }
    const int64_t want = (sc->recv_at[i] - expected[i]) / 1000;
    hosttest_log("  %-13s logged +%lld us, received +%lld us, %lld ms ago",
                 entry_names[i], (long long)(expected[i] - t0),
                 (long long)(sc->recv_at[i] - t0), (long long)sc->ms_ago[i]);

    // ms_ago is truncated to whole ms and the frame takes a little
    // virtual time to arrive, so allow a couple of ms either side. The
    // drift alone puts the probe 10 ms in the future, which used to wrap
    fails += !CHECK(sc->ms_ago[i] >= want - 2 && sc->ms_ago[i] <= want + 2,
                    "%s said %lld ms ago, expected %lld", entry_names[i],
                    (long long)sc->ms_ago[i], (long long)want);
  }
  return fails;
}

HOSTTEST_SUITE("evlog", test_evlog, 0);
