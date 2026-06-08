#include <mios/cmdline.h>
#include <mios/eventlog.h>

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "util/crc32.h"

struct cmdline_header {
  uint32_t magic;
  uint32_t length;
};

// malloc'd buffer of NUL-separated tokens, with a trailing NUL at
// [cmdline_len]. NULL/0 until a valid blob has been ingested.
static char *cmdline_buf;
static size_t cmdline_len;

void
cmdline_init(long addr, size_t maxlen)
{
  void *base = (void *)addr;
  struct cmdline_header hdr;
  memcpy(&hdr, base, sizeof(hdr));

  if(hdr.magic != CMDLINE_MAGIC)
    goto wipe_header;

  const uint32_t len = hdr.length;
  if(len == 0 || len > maxlen)
    goto wipe_header;

  // The crc covers the header and the string and is appended as its
  // complement, so the crc over the whole blob has a fixed residue and
  // ~crc32() is zero when valid (same scheme as VLLP).
  const size_t total = sizeof(hdr) + len + sizeof(uint32_t);
  if(~crc32(0, base, total) != 0)
    goto wipe_header;

  // MEM_MAY_FAIL so a failed allocation returns NULL (mios malloc() would
  // panic) and we just degrade to having no cmdline.
  char *buf = xalloc(len + 1, 0, MEM_MAY_FAIL);
  if(buf != NULL) {
    const char *str = (const char *)base + sizeof(hdr);
    memcpy(buf, str, len);
    buf[len] = '\0';

    evlog(LOG_NOTICE, "cmdline: %s", buf);

    // Tokenize: item separators become NUL so each value is a
    // ready-to-use C string.
    for(size_t i = 0; i < len; i++) {
      if(buf[i] == ' ')
        buf[i] = '\0';
    }
    cmdline_buf = buf;
    cmdline_len = len;
  }

  // Clear the crc so a stale blob is not re-validated after a reboot.
  memset((char *)base + sizeof(hdr) + len, 0, sizeof(uint32_t));

 wipe_header:
  // Clearing the magic alone invalidates the blob, but wipe the whole
  // header so nothing leaks across a reboot.
  memset(base, 0, sizeof(hdr));
}

// Return a pointer to the value of 'key', or NULL if not present.
// For a bare flag the returned pointer is to the token's trailing NUL
// (an empty string), which is still non-NULL.
static const char *
cmdline_find(const char *key)
{
  const size_t keylen = strlen(key);
  size_t i = 0;

  while(i < cmdline_len) {
    const char *tok = cmdline_buf + i;
    const size_t toklen = strlen(tok);
    if(toklen == 0) {
      i++; // skip empty token (e.g. repeated separators)
      continue;
    }
    if(!strncmp(tok, key, keylen)) {
      if(tok[keylen] == '=')
        return tok + keylen + 1;
      if(tok[keylen] == '\0')
        return tok + keylen;
    }
    i += toklen + 1;
  }
  return NULL;
}

const char *
cmdline_get_str(const char *key, const char *def)
{
  const char *v = cmdline_find(key);
  return v ? v : def;
}

int
cmdline_get_int(const char *key, int def)
{
  const char *v = cmdline_find(key);
  return v ? (int)atoix(v) : def;
}

int
cmdline_get_bool(const char *key)
{
  return cmdline_find(key) != NULL;
}
