/* Simple public domain implementation of the standard CRC32 checksum.
 * Outputs the checksum for each file given as a command line argument.
 * Invalid file names and files that cause errors are silently skipped.
 * The program reads from stdin if it is called with no arguments. */

// From http://home.thep.lu.se/~bjorn/crc/


#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <mios/task.h>

#include "crc32.h"

static uint32_t
crc32_for_byte(uint32_t r)
{
  for(int j = 0; j < 8; ++j)
    r = (r & 1? 0: (uint32_t)0xEDB88320L) ^ r >> 1;
  return r ^ (uint32_t)0xFF000000L;
}


uint32_t  __attribute__((weak))
crc32(const void *data, size_t n_bytes)
{
  static mutex_t mutex = MUTEX_INITIALIZER("crcinit");
  static uint32_t *table;

  mutex_lock(&mutex);
  if(!table) {
    table = malloc(sizeof(uint32_t) * 256);
    for(size_t i = 0; i < 0x100; ++i)
      table[i] = crc32_for_byte(i);
  }
  mutex_unlock(&mutex);

  uint32_t crc = 0;
  for(size_t i = 0; i < n_bytes; ++i)
    crc = table[(uint8_t)crc ^ ((uint8_t*)data)[i]] ^ crc >> 8;

  return crc;
}
