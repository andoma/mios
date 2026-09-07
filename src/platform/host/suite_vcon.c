/*
 * vcon: the virtual console (src/util/vcon.c, src/util/vcon_ring.h) and its
 * shell commands (src/shell/cmd_vcon.c), in virtual time.
 *
 * ENABLE_VCON is off by default and no in-tree target switched it on, so
 * until now none of this code had ever been executed by CI. It is about to
 * become the far end of a VLLP client link, so pin the behaviour down
 * first -- every claim vcon.h makes about scrollback replay, cursor
 * resync, multi-client mirroring and input merging.
 *
 * A test terminal (testterm.h) stands in for whatever a user attaches
 * with: it buffers both directions and never blocks the writer, so the
 * suite can inject keystrokes and inspect console output without running
 * in lockstep with the thread under test.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/param.h>

#include <mios/vcon.h>
#include <mios/stream.h>
#include <mios/task.h>
#include <mios/cli.h>

#include "util/vcon_ring.h"

#include "hosttest.h"
#include "testterm.h"

#define SEC 1000000ull

static int fails;

#define VCHECK(cond, ...)                                        \
  do { if(!(cond)) { fails++;                                    \
       hosttest_check(0, __FILE__, __LINE__, __VA_ARGS__); } } while(0)


/* ---------------- helpers ---------------- */

/* Drain everything currently pending for a client into buf. Loops because
   one call only yields what the ring can hand over contiguously. */
static size_t
drain_client(vcon_client_t *vcc, void *buf, size_t size)
{
  size_t off = 0;
  while(off < size) {
    size_t n = vcon_client_output(vcc, buf + off, size - off);
    if(n == 0)
      break;
    off += n;
  }
  return off;
}


/* Read from the backend with a deadline. Never use a blocking
   stream_read(..., required) here: if the thing under test stops pumping,
   an unbounded read turns a failed assertion into a hung run, which in CI
   means a wedged job instead of a message naming the broken check. Found
   the hard way -- a mutant that swallowed keystrokes hung this suite
   instead of failing it. */
static size_t
backend_read(vcon_t *vc, void *buf, size_t want, uint64_t timeout)
{
  const uint64_t deadline = clock_get() + timeout;
  size_t off = 0;
  while(off < want) {
    ssize_t n = stream_read(vcon_backend(vc), buf + off, want - off, 0);
    if(n > 0) {
      off += n;
      continue;
    }
    if(clock_get() >= deadline)
      break;
    usleep(10000);
  }
  return off;
}


static void
backend_puts(vcon_t *vc, const char *str)
{
  stream_write(vcon_backend(vc), str, strlen(str), 0);
}


/* ---------------- phases ---------------- */

/* vcon_ring.h advertises itself as dependency-free and host-testable, so
   test it directly: the drop-oldest and wraparound arithmetic is where an
   off-by-one silently corrupts scrollback rather than crashing. */
static void
phase_ring(void)
{
  hosttest_log("-- ring");

  /* Allocated, not a local union: vcon_ring_t ends in a flexible array and
     GCC rightly flags writes past a stack object whose real size it can
     see. vcon.c folds the storage into a calloc'd block, so do the same. */
  vcon_ring_t *r = calloc(1, sizeof(vcon_ring_t) + 8);
  uint8_t out[32];

  r->size = 8;

  vcon_ring_append(r, (const uint8_t *)"abc", 3);
  VCHECK(r->used == 3, "ring: used %zu after 3 bytes", r->used);
  vcon_ring_copy(r, 3, out, 3);
  VCHECK(!memcmp(out, "abc", 3), "ring: copy mismatch");

  /* Overflow by 2: oldest two bytes ("ab") must be the ones lost. */
  vcon_ring_append(r, (const uint8_t *)"defghij", 7);
  VCHECK(r->used == 8, "ring: used %zu after overflow", r->used);
  vcon_ring_copy(r, 8, out, 8);
  VCHECK(!memcmp(out, "cdefghij", 8), "ring: overflow kept '%.8s'", out);

  /* Partial read from the middle of the window. */
  vcon_ring_copy(r, 5, out, 3);
  VCHECK(!memcmp(out, "fgh", 3), "ring: mid-window copy got '%.3s'", out);

  /* Two segments, oldest first, must reassemble to the same thing. */
  const uint8_t *p0, *p1;
  size_t l0, l1;
  vcon_ring_segments(r, &p0, &l0, &p1, &l1);
  VCHECK(l0 + l1 == 8, "ring: segments total %zu", l0 + l1);
  memcpy(out, p0, l0);
  memcpy(out + l0, p1, l1);
  VCHECK(!memcmp(out, "cdefghij", 8), "ring: segments gave '%.8s'", out);

  /* A single append larger than the ring: only the tail survives, and the
     implementation takes a different code path to lay it out. */
  vcon_ring_append(r, (const uint8_t *)"0123456789ABCDEFGHIJ", 20);
  VCHECK(r->used == 8, "ring: used %zu after oversize", r->used);
  vcon_ring_copy(r, 8, out, 8);
  VCHECK(!memcmp(out, "CDEFGHIJ", 8), "ring: oversize kept '%.8s'", out);

  /* Exactly-full append, which is the boundary between the two paths. */
  memset(r, 0, sizeof(vcon_ring_t) + 8);
  r->size = 8;
  vcon_ring_append(r, (const uint8_t *)"01234567", 8);
  VCHECK(r->used == 8, "ring: used %zu after exact fill", r->used);
  vcon_ring_copy(r, 8, out, 8);
  VCHECK(!memcmp(out, "01234567", 8), "ring: exact fill gave '%.8s'", out);

  free(r);
  /* The size==0 ring is covered through the real API in
     phase_no_scrollback(): poking it directly from here means handing
     vcon_ring_append() an object with nothing behind buf[], which GCC
     diagnoses at every allocation size we could pick. */
}


/* A console with no scrollback at all. Legal (vcon_create takes the size
   from the caller) and it drives the ring's size==0 guards, which are
   otherwise dead code. Nothing is buffered, so a client only ever sees
   what is written while it is attached. */
static void
phase_no_scrollback(void)
{
  hosttest_log("-- no scrollback");

  vcon_t *vc = vcon_create("c-nosb", 0, 64);
  VCHECK(vc != NULL, "nosb: create failed");
  if(vc == NULL)
    return;

  backend_puts(vc, "dropped-on-the-floor");
  VCHECK(vcon_scrollback_used(vc) == 0, "nosb: scrollback used %zu, want 0",
         vcon_scrollback_used(vc));

  testterm_t *tt = testterm_create();
  vcon_client_t *vcc = vcon_attach(vc, testterm_stream(tt));
  char buf[64];
  VCHECK(drain_client(vcc, buf, sizeof(buf)) == 0,
         "nosb: replayed history that was never stored");

  /* Live output is still accounted for, it just is not retained. */
  backend_puts(vc, "live");
  VCHECK(drain_client(vcc, buf, sizeof(buf)) == 0,
         "nosb: returned bytes from a zero-size ring");

  /* Input is unaffected -- that fifo is a separate buffer. */
  VCHECK(vcon_input(vc, "k", 1) == 1, "nosb: input rejected");
  size_t n = backend_read(vc, buf, 1, 2 * SEC);
  VCHECK(n == 1 && buf[0] == 'k', "nosb: backend read %zu", n);

  vcon_detach(vcc);
  free(tt);
}


static void
phase_basic(void)
{
  hosttest_log("-- basic");

  vcon_t *vc = vcon_create("c-basic", 256, 64);
  VCHECK(vc != NULL, "basic: create failed");
  if(vc == NULL)
    return;

  backend_puts(vc, "hello");
  VCHECK(vcon_scrollback_used(vc) == 5, "basic: scrollback %zu, want 5",
         vcon_scrollback_used(vc));
  VCHECK(vcon_client_count(vc) == 0, "basic: %d clients before attach",
         vcon_client_count(vc));

  testterm_t *tt = testterm_create();
  vcon_client_t *vcc = vcon_attach(vc, testterm_stream(tt));
  VCHECK(vcc != NULL, "basic: attach failed");
  if(vcc == NULL)
    return;

  VCHECK(vcon_client_count(vc) == 1, "basic: %d clients after attach",
         vcon_client_count(vc));

  /* Attaching starts the cursor at the oldest buffered byte, so output
     written before the attach is replayed. */
  char buf[64];
  size_t n = drain_client(vcc, buf, sizeof(buf));
  VCHECK(n == 5 && !memcmp(buf, "hello", 5),
         "basic: replay got %zu bytes '%.*s'", n, (int)n, buf);

  /* Nothing pending now. */
  VCHECK(vcon_client_output(vcc, buf, sizeof(buf)) == 0,
         "basic: output pending after drain");

  backend_puts(vc, "world");
  n = drain_client(vcc, buf, sizeof(buf));
  VCHECK(n == 5 && !memcmp(buf, "world", 5),
         "basic: live got %zu bytes '%.*s'", n, (int)n, buf);

  /* Keystrokes go the other way, out of the backend's read side. */
  VCHECK(vcon_input(vc, "ping", 4) == 4, "basic: input short");
  n = backend_read(vc, buf, 4, 2 * SEC);
  VCHECK(n == 4 && !memcmp(buf, "ping", 4),
         "basic: backend read got %zu '%.*s'", n, (int)n, buf);

  vcon_detach(vcc);
  VCHECK(vcon_client_count(vc) == 0, "basic: %d clients after detach",
         vcon_client_count(vc));
  free(tt);
}


/* A client that attaches after more output than the scrollback can hold
   sees the most recent window, not the beginning. */
static void
phase_scrollback(void)
{
  hosttest_log("-- scrollback");

  vcon_t *vc = vcon_create("c-sb", 16, 64);
  char src[40];
  for(size_t i = 0; i < sizeof(src); i++)
    src[i] = '0' + (i % 10);
  stream_write(vcon_backend(vc), src, sizeof(src), 0);

  VCHECK(vcon_scrollback_used(vc) == 16, "scrollback: used %zu, want 16",
         vcon_scrollback_used(vc));

  testterm_t *tt = testterm_create();
  vcon_client_t *vcc = vcon_attach(vc, testterm_stream(tt));
  char buf[64];
  size_t n = drain_client(vcc, buf, sizeof(buf));
  VCHECK(n == 16, "scrollback: replayed %zu bytes, want 16", n);
  VCHECK(!memcmp(buf, src + sizeof(src) - 16, 16),
         "scrollback: replayed '%.*s', want '%.16s'", (int)n, buf,
         src + sizeof(src) - 16);

  vcon_detach(vcc);
  free(tt);
}


/* A client that stops draining while the producer runs ahead must skip the
   gap it lost and resume cleanly -- not read stale bytes, not read the same
   byte twice, not walk off the ring. */
static void
phase_lag(void)
{
  hosttest_log("-- lagging client");

  vcon_t *vc = vcon_create("c-lag", 16, 64);
  testterm_t *tt = testterm_create();
  vcon_client_t *vcc = vcon_attach(vc, testterm_stream(tt));

  char src[40];
  for(size_t i = 0; i < sizeof(src); i++)
    src[i] = 'A' + (i % 26);

  /* Write 10, consume 4. Cursor is at 4, well inside the window. */
  stream_write(vcon_backend(vc), src, 10, 0);
  char buf[64];
  size_t n = vcon_client_output(vcc, buf, 4);
  VCHECK(n == 4 && !memcmp(buf, src, 4), "lag: first read '%.*s'", (int)n, buf);

  /* Now run the producer 30 bytes ahead without draining. 40 bytes total
     written, a 16 byte window, so the oldest surviving byte is #24 and the
     cursor (at 4) has fallen 20 bytes behind the window. */
  stream_write(vcon_backend(vc), src + 10, 30, 0);

  n = drain_client(vcc, buf, sizeof(buf));
  VCHECK(n == 16, "lag: after resync got %zu bytes, want 16", n);
  VCHECK(!memcmp(buf, src + 24, 16),
         "lag: after resync got '%.*s', want '%.16s'", (int)n, buf, src + 24);

  /* And the stream continues correctly from there. */
  stream_write(vcon_backend(vc), "tail", 4, 0);
  n = drain_client(vcc, buf, sizeof(buf));
  VCHECK(n == 4 && !memcmp(buf, "tail", 4),
         "lag: continuation got %zu '%.*s'", n, (int)n, buf);

  vcon_detach(vcc);
  free(tt);
}


/* vcon.h promises shared sessions: output mirrored to every client, input
   from all of them merged into one backend stream. */
static void
phase_multi(void)
{
  hosttest_log("-- multi-client");

  vcon_t *vc = vcon_create("c-multi", 256, 64);
  testterm_t *ta = testterm_create();
  testterm_t *tb = testterm_create();

  vcon_client_t *a = vcon_attach(vc, testterm_stream(ta));
  vcon_client_t *b = vcon_attach(vc, testterm_stream(tb));
  VCHECK(vcon_client_count(vc) == 2, "multi: %d clients", vcon_client_count(vc));

  backend_puts(vc, "mirrored");

  char bufa[64], bufb[64];
  size_t na = drain_client(a, bufa, sizeof(bufa));
  size_t nb = drain_client(b, bufb, sizeof(bufb));
  VCHECK(na == 8 && !memcmp(bufa, "mirrored", 8),
         "multi: client A got %zu '%.*s'", na, (int)na, bufa);
  VCHECK(nb == 8 && !memcmp(bufb, "mirrored", 8),
         "multi: client B got %zu '%.*s'", nb, (int)nb, bufb);

  /* Each client's cursor is independent: a second client attaching later
     replays the same history the first one already consumed. */
  testterm_t *tc = testterm_create();
  vcon_client_t *c = vcon_attach(vc, testterm_stream(tc));
  char bufc[64];
  size_t nc = drain_client(c, bufc, sizeof(bufc));
  VCHECK(nc == 8 && !memcmp(bufc, "mirrored", 8),
         "multi: late client got %zu '%.*s'", nc, (int)nc, bufc);

  /* Merged input. Interleaving between the two is not specified, but every
     byte must arrive exactly once. */
  vcon_input(vc, "aaa", 3);
  vcon_input(vc, "bbb", 3);
  char in[8];
  size_t n = backend_read(vc, in, 6, 2 * SEC);
  VCHECK(n == 6, "multi: merged read %zu, want 6", n);
  int na_cnt = 0, nb_cnt = 0;
  for(size_t i = 0; i < n; i++) {
    if(in[i] == 'a') na_cnt++;
    else if(in[i] == 'b') nb_cnt++;
  }
  VCHECK(na_cnt == 3 && nb_cnt == 3,
         "multi: merged got %d a's and %d b's from '%.*s'", na_cnt, nb_cnt,
         (int)n, in);

  vcon_detach(a);
  vcon_detach(b);
  vcon_detach(c);
  VCHECK(vcon_client_count(vc) == 0, "multi: %d clients after detach",
         vcon_client_count(vc));
  free(ta);
  free(tb);
  free(tc);
}


/* The input fifo is small and bounded; a flood must be truncated, not
   overrun the buffer or wrap silently. */
static void
phase_input_limits(void)
{
  hosttest_log("-- input limits");

  vcon_t *vc = vcon_create("c-in", 64, 8);

  VCHECK(vcon_input(vc, "0123456789ABCDEF", 16) == 8,
         "input: fifo accepted more than 8 bytes");
  VCHECK(vcon_input(vc, "x", 1) == 0, "input: accepted a byte while full");

  char buf[16];
  size_t n = backend_read(vc, buf, 8, 2 * SEC);
  VCHECK(n == 8 && !memcmp(buf, "01234567", 8),
         "input: drained %zu '%.*s'", n, (int)n, buf);

  /* Room again, and the ring indices wrapped in between. */
  VCHECK(vcon_input(vc, "abcdefgh", 8) == 8, "input: refill short");
  n = backend_read(vc, buf, 8, 2 * SEC);
  VCHECK(n == 8 && !memcmp(buf, "abcdefgh", 8),
         "input: after wrap got %zu '%.*s'", n, (int)n, buf);

  /* Non-blocking backend read on an empty fifo returns 0 rather than
     hanging -- this is the path the VLLP pull() callback will use. */
  n = stream_read(vcon_backend(vc), buf, sizeof(buf), 0);
  VCHECK(n == 0, "input: non-blocking read returned %zu on empty fifo", n);
}


static void
phase_registry(void)
{
  hosttest_log("-- registry");

  VCHECK(vcon_find("c-basic") != NULL, "registry: c-basic not found");
  VCHECK(vcon_find("c-lag") != NULL, "registry: c-lag not found");
  VCHECK(vcon_find("no-such-console") == NULL,
         "registry: found a console that was never created");

  vcon_t *found = vcon_find("c-multi");
  VCHECK(found != NULL && !strcmp(vcon_name(found), "c-multi"),
         "registry: name mismatch");

  int n = 0;
  int seen_basic = 0;
  for(vcon_t *vc = vcon_first(); vc != NULL; vc = vcon_next(vc)) {
    n++;
    if(!strcmp(vcon_name(vc), "c-basic"))
      seen_basic++;
    VCHECK(n < 100, "registry: iteration does not terminate");
    if(n >= 100)
      break;
  }
  VCHECK(seen_basic == 1, "registry: c-basic appears %d times in the list",
         seen_basic);
  hosttest_log("   %d consoles registered", n);
}


/* vcon_bind() is the "dedicated port" path: a thread of its own pumping
   both directions for the life of the system. */
static void
phase_bind(void)
{
  hosttest_log("-- bind");

  vcon_t *vc = vcon_create("c-bind", 256, 64);
  testterm_t *tt = testterm_create();

  vcon_bind(vc, testterm_stream(tt));

  backend_puts(vc, "to-the-port");
  VCHECK(testterm_out_wait(tt, "to-the-port", 2 * SEC),
         "bind: console output never reached the bound terminal");

  /* And terminal input reaches the backend. */
  testterm_types(tt, "typed");
  char buf[16];
  size_t n = backend_read(vc, buf, 5, 2 * SEC);
  VCHECK(n == 5 && !memcmp(buf, "typed", 5),
         "bind: backend got %zu '%.*s'", n, (int)n, buf);

  /* A bound terminal is a normal client, so others may attach alongside. */
  testterm_t *t2 = testterm_create();
  vcon_client_t *vcc = vcon_attach(vc, testterm_stream(t2));
  VCHECK(vcon_client_count(vc) == 2, "bind: %d clients with one attached",
         vcon_client_count(vc));
  backend_puts(vc, "both");
  VCHECK(testterm_out_wait(tt, "both", 2 * SEC), "bind: bound terminal missed it");
  char b2[64];
  size_t n2 = drain_client(vcc, b2, sizeof(b2));
  b2[MIN(n2, sizeof(b2) - 1)] = 0;
  VCHECK(n2 > 0 && testterm_contains(b2, "both"),
         "bind: attached client missed it (%zu bytes)", n2);
  vcon_detach(vcc);
  free(t2);
  /* tt stays alive: vcon_bind's thread owns it forever by design. */
}


/* vcon_create_shell(): a real MIOS shell on the backend. Runs `consoles`
   through it, which is a nice closed loop -- the shell reached over the
   vcon lists the vcon it is running on. */
static void
phase_shell(void)
{
  hosttest_log("-- shell on a vcon");

  vcon_t *vc = vcon_create_shell("c-shell", 4096, 64);
  VCHECK(vc != NULL, "shell: create failed");
  if(vc == NULL)
    return;

  testterm_t *tt = testterm_create();
  vcon_client_t *vcc = vcon_attach(vc, testterm_stream(tt));

  /* Pump the client in the background the way cmd_attach does, so the
     shell's output lands in the terminal as it is produced. */
  char buf[256];
  const uint64_t deadline = clock_get() + 5 * SEC;
  int saw_prompt = 0;
  while(clock_get() < deadline) {
    size_t n = vcon_client_output(vcc, buf, sizeof(buf));
    if(n) {
      stream_write(testterm_stream(tt), buf, n, 0);
      continue;
    }
    if(testterm_out_has(tt, ">")) {
      saw_prompt = 1;
      break;
    }
    usleep(10000);
  }
  VCHECK(saw_prompt, "shell: no prompt appeared");

  /* Ask the shell to list consoles. Its own name must come back. */
  testterm_out_clear(tt);
  vcon_input(vc, "consoles\n", 9);

  const uint64_t d2 = clock_get() + 5 * SEC;
  int saw_self = 0;
  while(clock_get() < d2) {
    size_t n = vcon_client_output(vcc, buf, sizeof(buf));
    if(n) {
      stream_write(testterm_stream(tt), buf, n, 0);
      continue;
    }
    if(testterm_out_has(tt, "c-shell")) {
      saw_self = 1;
      break;
    }
    usleep(10000);
  }
  VCHECK(saw_self, "shell: `consoles` output did not mention c-shell");

  vcon_detach(vcc);
  free(tt);
}


/* ---------------- the attach CLI command ---------------- */

typedef struct attach_ctx {
  testterm_t *tt;
  const char *console;
  volatile int running;
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
  ac->running = 1;
  ac->err = cli_dispatch(&cli, line);
  ac->done = 1;
  thread_exit(NULL);
}

static int
pred_done(void *arg)
{
  attach_ctx_t *ac = arg;
  return ac->done;
}


/* cmd_vcon.c: `consoles` and `attach`, including the ^A escape handling
   that decides whether a byte reaches the console or detaches the user. */
static void
phase_cli(void)
{
  hosttest_log("-- consoles / attach commands");

  vcon_t *vc = vcon_create("c-cli", 256, 64);

  /* `consoles` lists it. */
  testterm_t *tl = testterm_create();
  cli_t cli = { testterm_stream(tl) };
  char cmd[] = "consoles";
  cli_dispatch(&cli, cmd);
  VCHECK(testterm_out_has(tl, "c-cli"), "cli: `consoles` did not list c-cli");
  free(tl);

  /* `attach c-cli` in a thread of its own; it does not return until the
     user detaches. */
  attach_ctx_t ac = { .tt = testterm_create(), .console = "c-cli" };
  thread_create(attach_thread, &ac, 4096, "attach", TASK_DETACHED, 4);

  VCHECK(testterm_out_wait(ac.tt, "[attached to c-cli", 2 * SEC),
         "cli: attach banner never printed");

  /* Console output reaches the attached terminal. */
  backend_puts(vc, "REMOTE-OUTPUT");
  VCHECK(testterm_out_wait(ac.tt, "REMOTE-OUTPUT", 2 * SEC),
         "cli: console output did not reach the attached terminal");

  /* Ordinary keystrokes reach the console. */
  testterm_types(ac.tt, "hello");
  char buf[16];
  size_t n = backend_read(vc, buf, 5, 2 * SEC);
  VCHECK(n == 5 && !memcmp(buf, "hello", 5),
         "cli: console got %zu '%.*s'", n, (int)n, buf);

  /* ^A ^A is an escaped literal ^A -- one byte through, no detach. */
  testterm_types(ac.tt, "\x01\x01");
  n = backend_read(vc, buf, 1, 2 * SEC);
  VCHECK(n == 1 && buf[0] == 0x01,
         "cli: ^A^A delivered %zu bytes%s", n,
         n ? (buf[0] == 0x01 ? "" : " (wrong byte)") : " (nothing arrived)");
  VCHECK(!ac.done, "cli: ^A^A detached instead of sending a literal");

  /* ^A followed by an unrelated key delivers that key as-is. */
  testterm_types(ac.tt, "\x01z");
  n = backend_read(vc, buf, 1, 2 * SEC);
  VCHECK(n == 1 && buf[0] == 'z',
         "cli: ^A z delivered %zu bytes%s", n,
         n ? (buf[0] == 'z' ? "" : " (wrong byte)") : " (nothing arrived)");
  VCHECK(!ac.done, "cli: ^A z detached");

  /* ^A d detaches. */
  testterm_types(ac.tt, "\x01" "d");
  VCHECK(hosttest_wait(pred_done, &ac, 2 * SEC), "cli: ^A d did not detach");
  VCHECK(testterm_out_has(ac.tt, "[detached from c-cli]"),
         "cli: no detach message");
  VCHECK(ac.err == 0, "cli: attach returned %d", ac.err);
  VCHECK(vcon_client_count(vc) == 0,
         "cli: %d clients still attached after detach",
         vcon_client_count(vc));

  /* Attaching to something that does not exist fails cleanly. */
  testterm_t *tn = testterm_create();
  cli_t cli2 = { testterm_stream(tn) };
  char bad[] = "attach no-such-console";
  error_t err = cli_dispatch(&cli2, bad);
  VCHECK(err == ERR_NOT_FOUND, "cli: attach to a missing console returned %d",
         err);
  VCHECK(testterm_out_has(tn, "No such console"), "cli: no diagnostic printed");
  free(tn);

  free(ac.tt);
}


static int
test_vcon(void)
{
  hosttest_log("---- vcon ----");

  phase_ring();
  phase_no_scrollback();
  phase_basic();
  phase_scrollback();
  phase_lag();
  phase_multi();
  phase_input_limits();
  phase_registry();
  phase_bind();
  phase_shell();
  phase_cli();

  return fails;
}

HOSTTEST_SUITE("vcon", test_vcon, 0);
