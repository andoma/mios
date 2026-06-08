#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Boot command line.
 *
 * A blob can be written to a known RAM address before mios starts (for
 * example by the DFU/webflash tooling) to pass a "kernel cmdline" style
 * string into the firmware. The layout is:
 *
 *   uint32_t magic     (CMDLINE_MAGIC)
 *   uint32_t length    (string length in bytes, excluding terminating NUL)
 *   char     str[length]
 *   uint32_t crc32     (= ~crc32(0, &magic .. end of str), little-endian)
 *
 * The string is a space separated list of "key=value" items and bare
 * flags, e.g.  "usb.vid=0x1234 usb.pid=0x4567 cur8.assetid=foobar".
 *
 * The crc covers the header and the string and is appended as its bitwise
 * complement, so that running crc32() over the whole blob (header + string
 * + crc) yields the residue and ~crc32(...) == 0 for a valid blob. This is
 * the same scheme as the VLLP framing (see net/vllp.c). Note: crc32() here
 * is the mios crc32 variant (see util/crc32.c); the writer must use the
 * identical algorithm.
 */

#define CMDLINE_MAGIC 0x6c646d63 // 'cmdl'

// Descriptor exported into the ELF (section .cmdline_info) so host tooling
// (the dfu flasher) can locate where to deposit a cmdline blob for this
// build. 'addr' is the RAM address read by cmdline_init(); 'size' is the
// maximum accepted string length.
struct cmdline_info {
  uint32_t addr;
  uint32_t size;
};

// Declare the cmdline region for a platform. Use once at file scope, then
// pass mios_cmdline_info.addr / .size to cmdline_init().
#define CMDLINE_AT(a, s)                                                       \
  const struct cmdline_info __attribute__((section("cmdline_info"),            \
                                           used)) mios_cmdline_info = {(a), (s)}

extern const struct cmdline_info mios_cmdline_info;

/*
 * Validate and ingest a cmdline blob at 'addr'. 'maxlen' bounds the
 * accepted string length. On a valid blob the string is copied to a
 * malloc'd buffer and tokenized. The source region is always wiped (header
 * and crc cleared) so it cannot survive a reboot.
 *
 * Safe to call on garbage memory: validation simply fails and no cmdline
 * is registered. Called from platform init, after the heap is available
 * but before the RAM holding the blob is reused (e.g. by pbuf).
 */
void cmdline_init(long addr, size_t maxlen);

// Return the value of "key=value", or 'def' if the key is absent.
const char *cmdline_get_str(const char *key, const char *def);

// As cmdline_get_str() but parsed as integer (decimal, or hex if 0x prefixed).
int cmdline_get_int(const char *key, int def);

// Return non-zero if 'key' is present (as a bare flag or "key=value").
int cmdline_get_bool(const char *key);
