#pragma once

/*
 * A stream_t standing in for the terminal a user attaches with.
 *
 * Unlike pipe() it buffers both directions and never blocks the writer, so
 * a suite can inject keystrokes and inspect console output without running
 * in lockstep with the thread under test. Supports poll() on the read side,
 * which is what vcon_client_wait() and cmd_attach() need.
 */

#include <stddef.h>
#include <stdint.h>

struct stream;

typedef struct testterm testterm_t;

testterm_t *testterm_create(void);

/* The stream to hand to vcon_attach(), cli_t.cl_stream, etc. */
struct stream *testterm_stream(testterm_t *tt);

/* Type at it. Returns bytes accepted (short if the input buffer is full). */
size_t testterm_type(testterm_t *tt, const void *buf, size_t len);
size_t testterm_types(testterm_t *tt, const char *str);

/* What the console has written to it. */
size_t testterm_out_get(testterm_t *tt, char *dst, size_t dstsize);
int testterm_out_has(testterm_t *tt, const char *needle);
size_t testterm_out_len(testterm_t *tt);
void testterm_out_clear(testterm_t *tt);

/* Wait until the console has written `needle`. Virtual time advances while
   we sleep, so this costs no real time. Returns 1 if it appeared. */
int testterm_out_wait(testterm_t *tt, const char *needle, uint64_t timeout);

/* The MIOS libc has no strstr(). */
int testterm_contains(const char *hay, const char *needle);
