/*
 * "snprintf": boundary behaviour of the libc in src/lib/libc/stdio.c.
 *
 * snprintf() writes at most 'size' bytes *including* the terminator, and
 * returns the length the output would have had. Getting the first part
 * wrong is a buffer overflow at every "snprintf(buf, sizeof(buf), ...)"
 * call site in the tree, so it is worth pinning down.
 */

#include <stdio.h>
#include <string.h>

#include "hosttest.h"

// Reached through volatile pointers so that the arguments are opaque to
// the compiler: it would otherwise fold these calls and warn about the
// deliberate truncation (-Wformat-truncation, which is an error here).
static const char *volatile ten = "0123456789";
static const char *volatile seven = "0123456";
static const char *volatile abc = "abc";

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

  return fails;
}

HOSTTEST_SUITE("snprintf", run_snprintf, 0);
