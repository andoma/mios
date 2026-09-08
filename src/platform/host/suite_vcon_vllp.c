/*
 * vcon-vllp: the actual usecase, end to end, in virtual time.
 *
 * A gateway sits on a CAN bus with nine units. Each unit runs a mios VLLP
 * server with the ordinary "shell" service. The gateway is a VLLP client
 * on nine links; each link has a bind that keeps a channel open to the
 * remote shell, wired to a local virtual console. An operator telnets to
 * the gateway and types `attach unit3`.
 *
 * Everything in this suite is shipping code: src/net/vllp.c on both ends,
 * src/net/service/svc_shell.c on the unit side, src/net/vcon_pushpull.c
 * and src/util/vcon.c on the gateway side, src/shell/cmd_vcon.c for the
 * attach itself. The only test scaffolding is the wire (vcan_loop.h) and
 * the terminal (testterm.h).
 *
 * Both ends live in this process, so the "units" are the same mios
 * instance the gateway runs in. That is invisible to the protocol -- the
 * frames really do go out over the vcan and come back -- but it does mean
 * the process has nine shell threads that a real gateway would not.
 *
 * Gateway-side wiring is deliberately written the way a board init file
 * would write it, because that wiring is itself part of what is under
 * test: mios ships vllp_client_bind() and vcon_pushpull_open() as
 * separate primitives and leaves the joining to the application.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/vllp.h>
#include <mios/vcon.h>
#include <mios/vcon_pushpull.h>
#include <mios/pushpull.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/cli.h>

#include "net/pbuf.h"

#include "hosttest.h"
#include "sim.h"
#include "vcan.h"
#include "vcan_loop.h"
#include "testterm.h"

#define SEC 1000000ull

#define MTU 64
#define UNITS 9

/* Link i: 0x200+2i gateway->unit, 0x201+2i unit->gateway. Opposite ids at
   the two ends, so the loopback delivers each frame to exactly one. */
#define GW_TX(i) (0x200 + (i) * 2)
#define GW_RX(i) (0x201 + (i) * 2)

static int fails;

#define GCHECK(cond, ...)                                        \
  do { if(!(cond)) { fails++;                                    \
       hosttest_check(0, __FILE__, __LINE__, __VA_ARGS__); } } while(0)


typedef struct unit {
  char name[16];
  vcon_t *vcon;         /* gateway side */
  vllp_t *client;       /* gateway side */
  vllp_t *server;       /* the unit */
  vllp_bind_t *bind;
} unit_t;

static unit_t units[UNITS];
static vcan_loop_t *g_lb;

/* Set when this registration forces allocation failures (see the bottom
   of the file). A few phases can only assert what they assert when
   buffers are actually available. */
static int g_starved;


/* ---------------- gateway wiring, board-init style ---------------- */

/* vllp_client_bind() calls this for every new session and expects a fresh
   app bound to the channel. Binding a vcon is one call. */
static error_t
unit_console_open(void *opaque, pushpull_t *pp)
{
  return vcon_pushpull_open(opaque, pp);
}


static void
gateway_setup(void)
{
  for(int i = 0; i < UNITS; i++) {
    unit_t *u = &units[i];
    snprintf(u->name, sizeof(u->name), "unit%d", i + 1);

    /* The unit: a plain VLLP server offering whatever services it has
       (here that includes the real "shell"). */
    u->server = vllp_server_create(GW_RX(i), GW_TX(i), MTU, 3);

    /* The gateway: a console, a client link, and a bind joining them. */
    u->vcon = vcon_create(u->name, 4096, 128);
    u->client = vllp_client_create(GW_TX(i), GW_RX(i), MTU, 3);
    u->bind = vllp_client_bind(u->client, "shell", unit_console_open,
                               u->vcon);

    GCHECK(u->server && u->vcon && u->client && u->bind,
           "%s: setup failed", u->name);
  }
}


/* ---------------- an attached operator ---------------- */

/* Attaches to a vcon and pumps it the way cmd_attach() does, in a thread,
   so console output reaches the terminal as it is produced. Phases that
   test cmd_attach() itself use the real command instead. */
typedef struct viewer {
  testterm_t *tt;
  vcon_t *vc;
  vcon_client_t *vcc;
  volatile int stop;
  volatile int stopped;
} viewer_t;


__attribute__((noreturn))
static void *
viewer_thread(void *arg)
{
  viewer_t *vw = arg;
  uint8_t buf[128];

  while(!vw->stop) {
    size_t n = vcon_client_output(vw->vcc, buf, sizeof(buf));
    if(n) {
      stream_write(testterm_stream(vw->tt), buf, n, 0);
      continue;
    }
    vcon_client_wait(vw->vcc);
  }
  vw->stopped = 1;
  thread_exit(NULL);
}


static viewer_t *
viewer_attach(vcon_t *vc)
{
  viewer_t *vw = calloc(1, sizeof(viewer_t));
  vw->tt = testterm_create();
  vw->vc = vc;
  vw->vcc = vcon_attach(vc, testterm_stream(vw->tt));
  thread_create(viewer_thread, vw, 4096, "viewer", TASK_DETACHED, 4);
  return vw;
}


/* Stop the pump and detach. Both, and in that order: stopping the thread
   alone leaves the client attached, which then shows up as a bogus
   vcon_client_count() several phases later. */
static void
viewer_detach(viewer_t *vw)
{
  vw->stop = 1;

  /* The pump is asleep in vcon_client_wait(), which polls the terminal's
     read side. One byte of terminal input wakes it; nothing ever reads
     the viewer's terminal, so the byte is inert. */
  testterm_type(vw->tt, "\0", 1);

  const uint64_t deadline = clock_get() + 5 * SEC;
  while(!vw->stopped && clock_get() < deadline)
    usleep(10000);

  vcon_detach(vw->vcc);
}


/* Type at the console the way an attached client does. */
static void
viewer_type(vcon_t *vc, const char *str)
{
  vcon_input(vc, str, strlen(str));
}


/* Run a command on a console and wait for it to come back, retrying.
 *
 * Two things make the naive version unreliable, and both are by design
 * rather than bugs:
 *
 *  - A viewer's cursor starts at the oldest buffered byte, so attaching
 *    replays the whole scrollback. Waiting for "[connected]" or a prompt
 *    therefore matches history, not the session that is up now. Every tag
 *    here is unique across the run, so a match can only be a real round
 *    trip.
 *
 *  - vcon_pushpull_open() flushes queued input for each new session,
 *    because what is queued was typed at a shell that no longer exists.
 *    A command typed during a reconnect window is discarded on purpose,
 *    so the test has to retry rather than race it -- which is exactly
 *    what an operator does.
 */
static uint32_t g_tag_seq;

static int
console_command(viewer_t *vw, vcon_t *vc, const char *what, char *tagout,
                size_t tagsize, uint64_t timeout)
{
  const uint64_t deadline = clock_get() + timeout;

  while(clock_get() < deadline) {
    char tag[40];
    snprintf(tag, sizeof(tag), "zz-%s-%u", what, ++g_tag_seq);
    if(tagout != NULL)
      snprintf(tagout, tagsize, "%s", tag);

    char line[48];
    snprintf(line, sizeof(line), "%s\n", tag);
    vcon_input(vc, line, strlen(line));

    /* A round trip is fast in virtual time; if it has not come back in a
       few seconds the session was not there and we try again. */
    const uint64_t attempt = clock_get() + 4 * SEC;
    while(clock_get() < attempt) {
      if(testterm_out_has(vw->tt, tag))
        return 1;
      usleep(10000);
    }
  }
  return 0;
}


/* ---------------- phases ---------------- */

/* Every link comes up and every bind opens a channel to the remote shell.
   vcon_pushpull_open() writes "[connected]" into the scrollback, which is
   the gateway-visible signal that the far end answered. */
static void
phase_connect(void)
{
  hosttest_log("-- connect (%d units)", UNITS);

  const uint64_t deadline = clock_get() + 30 * SEC;
  int up = 0;

  while(clock_get() < deadline) {
    up = 0;
    for(int i = 0; i < UNITS; i++) {
      if(vcon_scrollback_used(units[i].vcon) > 0)
        up++;
    }
    if(up == UNITS)
      break;
    usleep(10000);
  }

  GCHECK(up == UNITS, "connect: only %d of %d units came up", up, UNITS);
}


/* Attach, and see the remote shell's prompt. This is the whole path:
   cmd_attach -> vcon scrollback -> vcon_pushpull push -> VLLP client
   channel -> loopback -> VLLP server -> svc_shell -> cli_on_stream. */
static void
phase_prompt(void)
{
  hosttest_log("-- prompt from each remote shell");

  for(int i = 0; i < UNITS; i++) {
    viewer_t *vw = viewer_attach(units[i].vcon);

    GCHECK(testterm_out_wait(vw->tt, "[connected]", 10 * SEC),
           "%s: no connect marker in the scrollback", units[i].name);
    GCHECK(testterm_out_wait(vw->tt, ">", 20 * SEC),
           "%s: the remote shell never sent a prompt", units[i].name);

    viewer_detach(vw);
  }
}


/* Run a command on each unit and read the answer back. `consoles` is a
   convenient one: its output is distinctive, and it proves a real shell
   is parsing and dispatching on the far end rather than something merely
   echoing bytes. */
static void
phase_command(void)
{
  hosttest_log("-- run a command on each unit");

  viewer_t *vw[UNITS];

  for(int i = 0; i < UNITS; i++) {
    vw[i] = viewer_attach(units[i].vcon);
    GCHECK(testterm_out_wait(vw[i]->tt, ">", 20 * SEC),
           "%s: no prompt before the command", units[i].name);
    testterm_out_clear(vw[i]->tt);
    viewer_type(units[i].vcon, "consoles\n");
  }

  for(int i = 0; i < UNITS; i++) {
    /* Every unit is the same mios instance, so its console list contains
       all nine names. Look for this unit's own, which must be there. */
    GCHECK(testterm_out_wait(vw[i]->tt, units[i].name, 25 * SEC),
           "%s: `consoles` produced no usable output", units[i].name);
    viewer_detach(vw[i]);
  }
}


/* All nine driven at once with per-unit payloads. Catches a channel id,
   CRC IV or vcon mixed up between links -- the failure mode where unit 3
   answers on unit 7's console, which a one-link test cannot see. */
static void
phase_crosstalk(void)
{
  hosttest_log("-- crosstalk (%d units at once)", UNITS);

  viewer_t *vw[UNITS];
  char tag[UNITS][32];

  for(int i = 0; i < UNITS; i++) {
    vw[i] = viewer_attach(units[i].vcon);
    GCHECK(testterm_out_wait(vw[i]->tt, ">", 20 * SEC),
           "%s: no prompt", units[i].name);
    testterm_out_clear(vw[i]->tt);
  }

  /* A distinct, wrong-on-purpose command per unit. The shell echoes the
     line as it is typed and then complains about it, so the tag comes
     back either way -- what matters is *which* console it comes back on. */
  for(int i = 0; i < UNITS; i++) {
    snprintf(tag[i], sizeof(tag[i]), "zz-tag-%d-%d", i, i * 7 + 3);
    char line[48];
    snprintf(line, sizeof(line), "%s\n", tag[i]);
    viewer_type(units[i].vcon, line);
  }

  for(int i = 0; i < UNITS; i++) {
    GCHECK(testterm_out_wait(vw[i]->tt, tag[i], 30 * SEC),
           "%s: never saw its own tag '%s'", units[i].name, tag[i]);
  }

  /* Nothing may carry another unit's tag. */
  for(int i = 0; i < UNITS; i++) {
    for(int j = 0; j < UNITS; j++) {
      if(i == j)
        continue;
      GCHECK(!testterm_out_has(vw[i]->tt, tag[j]),
             "%s: saw %s's tag '%s' -- traffic crossed between links",
             units[i].name, units[j].name, tag[j]);
    }
  }

  for(int i = 0; i < UNITS; i++)
    viewer_detach(vw[i]);
}


/* Both directions busy at once on the same link.
 *
 * This is the phase that matters most for whether a console over VLLP is
 * usable at all. VLLP's sequence numbers are a single bit shared by the
 * whole link, and test/vllp/FINDINGS.md records an open finding that
 * concurrent bidirectional origination could desynchronise it until the
 * 3 s timeout. A console is exactly that: the far end is writing output
 * while the operator types. If this wedges, the feature does not work.
 */
static void
phase_bidir(void)
{
  hosttest_log("-- bidirectional: remote output while the operator types");

  unit_t *u = &units[0];
  viewer_t *vw = viewer_attach(u->vcon);
  GCHECK(testterm_out_wait(vw->tt, ">", 20 * SEC), "bidir: no prompt");
  testterm_out_clear(vw->tt);

  /* `help` on the far end produces a screenful, so output is streaming
     down while we push keystrokes up. */
  viewer_type(u->vcon, "help\n");

  int typed = 0;
  const uint64_t deadline = clock_get() + 20 * SEC;
  while(clock_get() < deadline && typed < 40) {
    viewer_type(u->vcon, "x");
    typed++;
    usleep(50000);
  }

  /* The link must still be up and still be carrying data in both
     directions: a fresh command has to complete after all that. */
  viewer_type(u->vcon, "\n");
  GCHECK(console_command(vw, u->vcon, "after-bidir", NULL, 0, 30 * SEC),
         "bidir: the link stopped carrying data after both ends "
         "originated at once -- this is the shared 1-bit sequence "
         "desynchronising (see test/vllp/FINDINGS.md)");

  GCHECK(!testterm_out_has(vw->tt, "[disconnected"),
         "bidir: the link dropped while both ends were busy");

  hosttest_log("   %d keystrokes while output was streaming", typed);
  viewer_detach(vw);
}


/* Keystroke latency on an idle link.
 *
 * A console that works but lags by a second per keypress is not usable,
 * and "works" is all the other phases check. The engine only pulls from
 * an app when something makes it run, and on an idle link that something
 * is the once-a-second keepalive -- so without
 * vcon_set_backend_notify() wiring a keystroke to PUSHPULL_EVENT_PULL, a
 * character waits up to a full keepalive interval before it is even
 * fragmented. This phase is what makes that hook load-bearing; it is
 * measured in virtual time, so it is a count of protocol round trips and
 * not of host speed.
 */
static void
phase_latency(void)
{
  if(g_starved) {
    // Not measurable here, and not a defect either: with allocations
    // being forced to fail, pull() legitimately returns NULL because it
    // could not get a buffer, and the keystroke then waits for the next
    // thing that runs the engine -- the keepalive. Asserting sub-200ms
    // latency under injected allocation failure would be asserting that
    // the pool is never empty.
    hosttest_log("-- keystroke latency (skipped: allocations are being "
                 "forced to fail)");
    return;
  }

  hosttest_log("-- keystroke latency on an idle link");

  unit_t *u = &units[6];
  viewer_t *vw = viewer_attach(u->vcon);
  GCHECK(console_command(vw, u->vcon, "warmup", NULL, 0, 30 * SEC),
         "latency: console not working before the measurement");

  /* Let the link go quiet, so the next thing to happen is our keystroke
     rather than traffic that was already in flight. */
  usleep(2 * SEC);

  uint64_t worst = 0;
  for(int i = 0; i < 5; i++) {
    char tag[40];
    snprintf(tag, sizeof(tag), "zz-lat-%u", ++g_tag_seq);
    char line[48];
    snprintf(line, sizeof(line), "%s\n", tag);

    const uint64_t t0 = clock_get();
    vcon_input(u->vcon, line, strlen(line));

    const uint64_t deadline = t0 + 5 * SEC;
    while(!testterm_out_has(vw->tt, tag) && clock_get() < deadline)
      usleep(1000);

    const uint64_t took = clock_get() - t0;
    if(!testterm_out_has(vw->tt, tag)) {
      GCHECK(0, "latency: round trip %d never completed", i);
      break;
    }
    if(took > worst)
      worst = took;
    usleep(2 * SEC);   /* back to idle for the next one */
  }

  hosttest_log("   worst round trip %u us", (unsigned)worst);

  /* The keepalive is 1 s. Anything approaching that means the keystroke
     waited for a timer instead of waking the engine. 200 ms leaves plenty
     of room for a slow multi-fragment round trip while still being far
     below the interval it must not be gated on. */
  GCHECK(worst < 200000,
         "latency: worst keystroke round trip was %u us. A keystroke is "
         "waiting for the 1 s keepalive to move it, which means nothing "
         "is waking the engine when a client types -- check that "
         "vcon_set_backend_notify() is wired to PUSHPULL_EVENT_PULL",
         (unsigned)worst);

  viewer_detach(vw);
}


/* Detach, let the far end talk, re-attach: the point of a vcon is that
   output produced while nobody was looking is still there. */
static void
phase_scrollback(void)
{
  hosttest_log("-- scrollback across a detach");

  unit_t *u = &units[1];

  /* The whole cycle is retried, not just the command: the command is
     issued while nobody is attached, so there is no way to see that it
     was lost (which under injected allocation failure it can be) other
     than by not finding its output afterwards. */
  for(int attempt = 0; attempt < 5; attempt++) {

    viewer_t *vw = viewer_attach(u->vcon);
    if(!console_command(vw, u->vcon, "sb-warmup", NULL, 0, 40 * SEC)) {
      viewer_detach(vw);
      continue;   /* console not responsive yet; try the cycle again */
    }
    viewer_detach(vw);

    /* Nobody attached. Make the far end produce something identifiable. */
    char tag[40];
    snprintf(tag, sizeof(tag), "zz-while-detached-%u", ++g_tag_seq);
    char line[48];
    snprintf(line, sizeof(line), "%s\n", tag);
    vcon_input(u->vcon, line, strlen(line));
    usleep(3 * SEC);

    viewer_t *vw2 = viewer_attach(u->vcon);
    const int replayed = testterm_out_wait(vw2->tt, tag, 20 * SEC);
    viewer_detach(vw2);

    if(replayed)
      return;   /* pass */
  }

  GCHECK(0, "scrollback: output produced while detached was never replayed "
         "to a client that attached afterwards");
}


/* Two operators on one console: output to both, keystrokes from both. */
static void
phase_multi_attach(void)
{
  hosttest_log("-- two operators on one console");

  unit_t *u = &units[2];
  viewer_t *a = viewer_attach(u->vcon);
  viewer_t *b = viewer_attach(u->vcon);

  GCHECK(vcon_client_count(u->vcon) == 2, "multi: %d clients",
         vcon_client_count(u->vcon));

  GCHECK(testterm_out_wait(a->tt, ">", 20 * SEC), "multi: A saw no prompt");
  GCHECK(testterm_out_wait(b->tt, ">", 20 * SEC), "multi: B saw no prompt");

  char tag[40];
  GCHECK(console_command(a, u->vcon, "shared", tag, sizeof(tag), 25 * SEC),
         "multi: A missed the output");
  GCHECK(testterm_out_wait(b->tt, tag, 25 * SEC),
         "multi: B missed the output -- output is not mirrored");

  viewer_detach(a);
  viewer_detach(b);
}


/* A unit reboots. The gateway must notice, mark the console, re-SYN,
   re-open the channel and give the operator a working shell again --
   without the operator having to detach. This is the thing that will
   actually happen in the field, repeatedly.
 */
static void
phase_reconnect(int round)
{
  hosttest_log("-- unit reboot / reconnect (round %d)", round);

  unit_t *u = &units[3];
  viewer_t *vw = viewer_attach(u->vcon);
  GCHECK(testterm_out_wait(vw->tt, ">", 20 * SEC), "reconnect: no prompt");
  testterm_out_clear(vw->tt);

  /* 3 s timeouts at both ends; 7 s of silence takes the session down. */
  vcan_loop_outage(g_lb, 7 * SEC);

  GCHECK(testterm_out_wait(vw->tt, "[disconnected", 10 * SEC),
         "reconnect: the console was not marked disconnected, so an "
         "operator cannot tell a dead unit from a quiet one");

  /* And it comes back by itself. */
  GCHECK(testterm_out_wait(vw->tt, "[connected]", 30 * SEC),
         "reconnect: the console never reconnected");

  /* A fresh shell on the far end, reachable without re-attaching. */
  GCHECK(console_command(vw, u->vcon, "after-reboot", NULL, 0, 40 * SEC),
         "reconnect: no working shell on the console after reconnecting");

  /* The other units were on the same bus and must have recovered too.
     Wait for each to show a prompt on the new session before typing:
     vcon_pushpull_open() flushes pending input for every new session (it
     was aimed at a shell that no longer exists), so a command injected
     during the reconnect window is discarded by design. Racing that would
     make this phase flaky rather than wrong. */
  for(int i = 0; i < UNITS; i++) {
    if(i == 3)
      continue;
    viewer_t *o = viewer_attach(units[i].vcon);
    GCHECK(console_command(o, units[i].vcon, "still-there", NULL, 0,
                           40 * SEC),
           "%s: console not usable after the bus outage", units[i].name);
    viewer_detach(o);
  }

  viewer_detach(vw);
}


/* The same thing through the real `attach` command, over a stream, the way
   a telnet session reaches it. Covers the bit the viewer_thread stands in
   for everywhere else: cmd_attach()'s own pump and its ^A escape. */
typedef struct attach_ctx {
  testterm_t *tt;
  const char *console;
  volatile int done;
  error_t err;
} attach_ctx_t;

__attribute__((noreturn))
static void *
attach_thread(void *arg)
{
  attach_ctx_t *ac = arg;
  char line[64];
  snprintf(line, sizeof(line), "attach %s", ac->console);
  cli_t cli = { testterm_stream(ac->tt) };
  ac->err = cli_dispatch(&cli, line);
  ac->done = 1;
  thread_exit(NULL);
}

static int
pred_attach_done(void *arg)
{
  attach_ctx_t *ac = arg;
  return ac->done;
}


static void
phase_attach_command(void)
{
  hosttest_log("-- the real `attach` command end to end");

  unit_t *u = &units[4];

  attach_ctx_t ac = { .tt = testterm_create(), .console = u->name };
  thread_create(attach_thread, &ac, 4096, "attach", TASK_DETACHED, 4);

  GCHECK(testterm_out_wait(ac.tt, "[attached to", 5 * SEC),
         "attach: no banner");
  GCHECK(testterm_out_wait(ac.tt, ">", 25 * SEC),
         "attach: no prompt from the remote shell through `attach`");

  /* Type at the terminal, not at the vcon: this goes through
     cmd_attach()'s escape handling on the way in. */
  testterm_out_clear(ac.tt);
  testterm_types(ac.tt, "zz-via-attach\n");
  GCHECK(testterm_out_wait(ac.tt, "zz-via-attach", 30 * SEC),
         "attach: keystrokes did not reach the remote shell");

  /* ^A d detaches, and leaves the console running for the next operator. */
  testterm_types(ac.tt, "\x01" "d");
  GCHECK(hosttest_wait(pred_attach_done, &ac, 5 * SEC),
         "attach: ^A d did not detach");
  GCHECK(testterm_out_has(ac.tt, "[detached from"),
         "attach: no detach message");
  GCHECK(vcon_client_count(u->vcon) == 0, "attach: %d clients left attached",
         vcon_client_count(u->vcon));

  /* Re-attaching gets a working console, and the scrollback is still
     there -- the far-end shell was never torn down by the detach. */
  viewer_t *vw = viewer_attach(u->vcon);
  GCHECK(testterm_out_wait(vw->tt, "zz-via-attach", 20 * SEC),
         "attach: scrollback lost after detach");
  GCHECK(console_command(vw, u->vcon, "after-detach", NULL, 0, 30 * SEC),
         "attach: console dead after a detach/re-attach cycle");
  viewer_detach(vw);
}


/* Consoles over a lossy bus. Loss and duplication must be ridden through
   by the link layer; corruption resets the session, so the console must
   come back rather than wedge. Either way an operator must never see
   corrupted output presented as real. */
static void
phase_faults(const char *name, int drop, int dup, int corrupt)
{
  hosttest_log("-- %s (drop=%d%% dup=%d%% corrupt=%d%%)", name, drop, dup,
               corrupt);

  unit_t *u = &units[5];
  viewer_t *vw = viewer_attach(u->vcon);
  GCHECK(testterm_out_wait(vw->tt, ">", 25 * SEC), "%s: no prompt to start",
         name);

  const uint32_t frames0 = g_lb->frames;
  const uint32_t dropped0 = g_lb->dropped;
  const uint32_t dup0 = g_lb->duplicated;
  const uint32_t corrupt0 = g_lb->corrupted;

  g_lb->drop_pct = drop;
  g_lb->dup_pct = dup;
  g_lb->corrupt_pct = corrupt;

  int ok = 0;
  for(int i = 0; i < 20; i++) {
    char tag[32];
    snprintf(tag, sizeof(tag), "zz-f%d-%d", corrupt, i);
    char line[48];
    snprintf(line, sizeof(line), "%s\n", tag);
    testterm_out_clear(vw->tt);
    viewer_type(u->vcon, line);
    if(testterm_out_wait(vw->tt, tag, 30 * SEC))
      ok++;
  }

  g_lb->drop_pct = 0;
  g_lb->dup_pct = 0;
  g_lb->corrupt_pct = 0;

  const uint32_t frames = g_lb->frames - frames0;
  const uint32_t dropped = g_lb->dropped - dropped0;
  const uint32_t duped = g_lb->duplicated - dup0;
  const uint32_t corrupted = g_lb->corrupted - corrupt0;

  /* Enough traffic for the configured rate to have bitten. Without this
     the phase passes just as happily when nothing was injected at all,
     which is indistinguishable from real coverage. */
  GCHECK(frames > 100, "%s: only %u frames crossed the bus -- too few for "
         "a %d%%/%d%%/%d%% fault rate to have done anything, so this phase "
         "proved nothing", name, frames, drop, dup, corrupt);
  if(drop)
    GCHECK(dropped > 0, "%s: %u frames at %d%% loss and not one was "
           "dropped -- faults are not reaching the wire", name, frames, drop);
  if(dup)
    GCHECK(duped > 0, "%s: %u frames at %d%% duplication and not one was "
           "duplicated -- faults are not reaching the wire", name, frames,
           dup);
  if(corrupt)
    GCHECK(corrupted > 0, "%s: %u frames at %d%% corruption and not one was "
           "corrupted -- faults are not reaching the wire", name, frames,
           corrupt);

  if(corrupt) {
    /* A CRC failure resets the link, so some commands are expected to be
       lost. What must hold is that it recovers. */
    GCHECK(ok > 0, "%s: nothing got through at all", name);
  } else if(g_starved) {
    // With allocations being forced to fail, a command can be lost
    // because a buffer was refused rather than because a frame was. The
    // claim that survives is progress, not perfection.
    GCHECK(ok >= 15, "%s: only %d of 20 commands got through even allowing "
           "for injected allocation failures", name, ok);
  } else {
    GCHECK(ok == 20, "%s: only %d of 20 commands got through -- loss and "
           "duplication should be invisible above the link layer", name, ok);
  }

  GCHECK(console_command(vw, u->vcon, "recovered", NULL, 0, 120 * SEC),
         "%s: console did not recover once the faults stopped", name);

  hosttest_log("   %d of 20 commands through; %u frames, %u dropped, "
               "%u duplicated, %u corrupted", ok, frames, dropped, duped,
               corrupted);

  viewer_detach(vw);
}


/* Nested attach: attach to a unit, and from that unit's shell attach to
   another. Two cmd_attach() loops are then in series on the same byte
   stream, and both filter the same escape prefix. */
static void
phase_nested(void)
{
  if(g_starved) {
    // What this phase checks is where an escape byte gets consumed, which
    // has nothing to do with buffer pressure. Establishing the nesting
    // does though: it takes a typed command through two hops, and with
    // allocations being forced to fail that command can simply be
    // dropped. The other two registrations cover the routing.
    hosttest_log("-- nested attach (skipped: allocations are being forced "
                 "to fail)");
    return;
  }

  hosttest_log("-- nested attach");

  attach_ctx_t ac = { .tt = testterm_create(), .console = units[7].name };
  thread_create(attach_thread, &ac, 4096, "attach", TASK_DETACHED, 4);

  GCHECK(testterm_out_wait(ac.tt, "[attached to unit8", 10 * SEC),
         "nested: outer attach did not start");
  GCHECK(testterm_out_wait(ac.tt, ">", 25 * SEC), "nested: no outer prompt");

  /* From the remote shell, attach to a second console. Everything we type
     now passes through the outer cmd_attach() on its way there. */
  testterm_out_clear(ac.tt);
  testterm_types(ac.tt, "attach unit9\n");
  GCHECK(testterm_out_wait(ac.tt, "[attached to unit9", 25 * SEC),
         "nested: inner attach did not start");

  /* The nesting has to actually carry traffic, in both directions and
     through both levels. Checking only that the inner attach *started*
     misses the case that mattered: the inner cmd_attach's terminal is a
     pushpull stream, which holds written data until a fragment fills or
     someone flushes, so console output used to sit in that buffer
     indefinitely -- keystrokes reached the inner console and its shell
     answered, but nothing ever came back up to the operator. */
  testterm_out_clear(ac.tt);
  testterm_types(ac.tt, "zz-through-both-levels\n");
  GCHECK(testterm_out_wait(ac.tt, "zz-through-both-levels", 30 * SEC),
         "nested: nothing came back through the nested session -- the "
         "inner terminal is buffering output that never gets flushed");

  /* A bare ^A is eaten by the OUTER loop -- it is the first filter the
     byte meets -- so the inner one never sees it. Doubling it makes the
     outer loop pass one through, which the inner loop then takes as its
     own prefix. So the inner detach is ^A ^A d, and the outer is ^A d. */
  testterm_out_clear(ac.tt);
  testterm_types(ac.tt, "\x01\x01" "d");
  GCHECK(testterm_out_wait(ac.tt, "[detached from unit9]", 15 * SEC),
         "nested: ^A^Ad did not detach the inner session");
  GCHECK(!ac.done, "nested: ^A^Ad detached the outer session too");
  GCHECK(vcon_client_count(units[8].vcon) == 0,
         "nested: %d clients left on unit9",
         vcon_client_count(units[8].vcon));

  /* Still attached to the outer one, and it still works. */
  viewer_t probe = { .tt = ac.tt };
  (void)probe;
  GCHECK(vcon_client_count(units[7].vcon) == 1,
         "nested: outer session lost, %d clients on unit8",
         vcon_client_count(units[7].vcon));

  /* And a single ^A d now detaches the outer one. */
  testterm_types(ac.tt, "\x01" "d");
  GCHECK(hosttest_wait(pred_attach_done, &ac, 10 * SEC),
         "nested: ^Ad did not detach the outer session");
  GCHECK(testterm_out_has(ac.tt, "[detached from unit8]"),
         "nested: no outer detach message");
}


/* ---------------- driver ---------------- */

static int
test_vcon_vllp(void)
{
  const int pool_total = pbuf_buffer_avail();
  const unsigned int fails_at_start = pbuf_alloc_fail_count();
  const int pool_at_start = pool_total;

  // The starved registration is the one that wants injected failures: a
  // small pool alone does not reliably produce them, which is checked at
  // the end.
  const int starved = pool_total <= 48;
  g_starved = starved;
#ifdef ENABLE_PBUF_FAULT_INJECT
  if(starved)
    pbuf_fault_inject(5, 1);
#endif

  hosttest_log("---- %d remote consoles over VLLP (pool %d buffers) ----%s",
               UNITS, pool_total,
               starved ? " [5% of allocations forced to fail]" : "");

  vcan_t *vcan = vcan_create("vcan0", MTU);
  gateway_setup();
  vcan_set_link(vcan, 1);
  g_lb = vcan_loop_create(vcan, 0x0badc0de);

  phase_connect();
  phase_prompt();
  phase_command();
  phase_crosstalk();
  phase_bidir();
  phase_latency();
  phase_scrollback();
  phase_multi_attach();
  phase_attach_command();
  phase_nested();
  phase_reconnect(1);
  phase_reconnect(2);
  phase_faults("loss-5", 5, 0, 0);
  phase_faults("dup-10", 0, 10, 0);
  /* 5%, not 1%: at 1% only one or two frames in a phase are hit,
     which is too thin to be sure the reset-and-recover path ran. */
  phase_faults("corrupt-5", 0, 0, 5);

  hosttest_log("  %u frames crossed the bus, %u dropped", g_lb->frames,
               g_lb->dropped);
  g_lb->stop = 1;

  const unsigned int allocfails = pbuf_alloc_fail_count() - fails_at_start;
  hosttest_log("  %u buffer allocation failures", allocfails);

  // Everything has been closed and the bus has been quiet; the pool must
  // be back where it started. Buffers still held here are leaked, and a
  // leak on a link-reset path is invisible until a device that has been
  // up for a month stops working.
  const int pool_now = pbuf_buffer_avail();
  hosttest_log("  pool %d of %d buffers free", pool_now, pool_at_start);
  GCHECK(pool_now >= pool_at_start - 2,
         "%d of %d buffers are still held after everything went idle -- "
         "something on the link-reset path is leaking them", 
         pool_at_start - pool_now, pool_at_start);

  // A starved run that never actually starved is a duplicate of the
  // others: passing, meaningless, and impossible to tell from real
  // coverage.
  if(starved)
    GCHECK(allocfails > 0, "no allocation ever failed -- this registration "
           "is meant to run under pressure, so it tested nothing");

  return fails;
}

// Registered three times, same scenarios, different pbuf pools. Nine
// concurrent links is a much harder allocation test than the single-link
// suites, and the paths that matter here are the ones that must cope with
// pbuf_make() returning NULL: vcon_pushpull.c's pull() (which peeks
// before allocating precisely so a failure loses no keystrokes) and the
// client's OPEN request, which has to stay queued rather than be dropped.
//
//   vcon-vllp          platform default (512 byte buffers, 256 of them)
//   vcon-vllp-tight    72 byte buffers, what a CAN-only target ends up
//                      with when sized for one max CAN-FD payload plus
//                      dsig's 4-byte signal id. Every frame nearly fills
//                      a buffer.
//   vcon-vllp-starved  72 byte buffers and only 40 of them, for nine
//                      links, with allocation failures injected on top.
HOSTTEST_SUITE_EX("vcon-vllp", test_vcon_vllp, 0, 0);
HOSTTEST_SUITE_EX("vcon-vllp-tight", test_vcon_vllp, 0, 72);
HOSTTEST_SUITE_POOL("vcon-vllp-starved", test_vcon_vllp, 0, 72, 40);
