/*
 * "snprintf": boundary behaviour of the libc in src/lib/libc/stdio.c.
 *
 * snprintf() writes at most 'size' bytes *including* the terminator, and
 * returns the length the output would have had. Getting the first part
 * wrong is a buffer overflow at every "snprintf(buf, sizeof(buf), ...)"
 * call site in the tree, so it is worth pinning down.
 *
 * Then the conversions: POSIX semantics, plus the two deliberate
 * extensions (a negative precision on %s hexdumps, %I formats an IPv4
 * address). A precision on %s used to emit exactly that many bytes
 * rather than at most, which reads off the end of the argument.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "hosttest.h"

// Reached through volatile pointers so that the arguments are opaque to
// the compiler: it would otherwise fold these calls and warn about the
// deliberate truncation (-Wformat-truncation, which is an error here).
static const char *volatile ten = "0123456789";
static const char *volatile seven = "0123456";
static const char *volatile abc = "abc";
static const char *volatile abcdef = "abcdef";

// No terminator: a precision is what makes this a legal %s argument
static const char raw4[4] = { 'a', 'b', 'c', 'd' };
static const char *volatile raw4p = raw4;

static const uint8_t bin3[3] = { 0x01, 0x02, 0x03 };
static const char *volatile bin3p = (const char *)bin3;

static volatile double inf, nan;


/* Every case is "this format with these arguments produces this string",
   and the return value has to agree with it. */
#define PCHECK(want, fmt, ...)                                          \
  do {                                                                  \
    memset(buf, 'X', sizeof(buf));                                      \
    const int r_ = snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__);      \
    fails += !CHECK(!strcmp(buf, want) && r_ == (int)strlen(want),       \
                    "\"%s\" gave '%s' (%d), want '%s' (%d)",              \
                    fmt, buf, r_, want, (int)strlen(want));             \
  } while(0)

static int
run_precision(void)
{
  int fails = 0;
  char buf[32];

  inf = 1e300;
  inf = inf * inf;
  nan = inf - inf;

  // A precision shorter than the string truncates
  PCHECK("abc", "%.3s", abcdef);

  // ...and one longer than it does not pad the output out to it. This is
  // the bug: emitting exactly the precision runs off the end of the
  // argument, so the caller's terminator (and whatever follows it) lands
  // in the middle of the result.
  PCHECK("abc", "%.8s", abc);
  PCHECK("[abc]", "[%.8s]", abc);

  // Exactly the precision, out of a buffer with no terminator at all
  PCHECK("abcd", "%.4s", raw4p);

  // A precision of zero emits nothing
  PCHECK("", "%.0s", abc);

  // Width pads whatever the precision left, on either side
  PCHECK("    ab|", "%6.2s|", abcdef);
  PCHECK("ab    |", "%-6.2s|", abcdef);
  PCHECK("   abc|", "%6.8s|", abc);

  // A precision applies to its own conversion and no other
  PCHECK("ababc", "%.2s%s", abcdef, abc);

  // Extension: a negative precision on %s hexdumps that many bytes, and
  // '-' separates them. Not POSIX; used by the version/build-id logging.
  PCHECK("010203", "%.*s", -3, bin3p);
  PCHECK("01.02.03", "%-.*s", -3, bin3p);

  // On a numeric conversion the precision is a digit count, so it must
  // not truncate the text those conversions fall back to either
  PCHECK("+inf", "%.3f", inf);
  PCHECK("nan", "%.1f", nan);

  return fails;
}

static int
run_snprintf(void)
{
  int fails = 0;
  char buf[16];

  // Truncating: 7 characters and a terminator, nothing beyond
  memset(buf, 'X', sizeof(buf));
  int r = snprintf(buf, 8, "%s", ten);
  fails += !CHECK(r == 10, "return should be the would-be length, got %d", r);
  fails += !CHECK(!strcmp(buf, "0123456"), "got '%s'", buf);
  fails += !CHECK(buf[8] == 'X', "wrote past the buffer (0x%02x)", buf[8]);

  // Exact fit: 7 characters and a terminator in 8 bytes
  memset(buf, 'X', sizeof(buf));
  r = snprintf(buf, 8, "%s", seven);
  fails += !CHECK(r == 7, "return should be 7, got %d", r);
  fails += !CHECK(!strcmp(buf, "0123456"), "got '%s'", buf);
  fails += !CHECK(buf[8] == 'X', "wrote past the buffer (0x%02x)", buf[8]);

  // Size 0: no write at all, but still report the length
  memset(buf, 'X', sizeof(buf));
  r = snprintf(buf, 0, "%s", abc);
  fails += !CHECK(r == 3, "return should be 3, got %d", r);
  fails += !CHECK(buf[0] == 'X', "wrote into a zero sized buffer");

  // Size 1: room for the terminator only
  memset(buf, 'X', sizeof(buf));
  r = snprintf(buf, 1, "%s", abc);
  fails += !CHECK(r == 3, "return should be 3, got %d", r);
  fails += !CHECK(buf[0] == 0, "buf[0] should be the terminator");
  fails += !CHECK(buf[1] == 'X', "wrote past the buffer (0x%02x)", buf[1]);

  fails += run_precision();

  return fails;
}

HOSTTEST_SUITE("snprintf", run_snprintf, 0);
