#pragma once

#include "error.h"

typedef enum {
  FLASH_SECTOR_TYPE_PROG  = 1, // .text, .data, -segments, etc
  FLASH_SECTOR_TYPE_PKV   = 2, // Persistent Key Value, see src/util/pkv.[ch]
  FLASH_SECTOR_TYPE_AVAIL = 3,
} flash_sector_type_t;


typedef struct {
  uint32_t src_offset;
  uint32_t dst_offset;
  uint32_t length;

} flash_multi_write_chunk_t;

typedef struct {
  uint32_t num_chunks;
  flash_multi_write_chunk_t chunks[0];

} flash_multi_write_chunks_t;



typedef struct flash_iface {

  size_t (*get_sector_size)(const struct flash_iface *fif, int sector);

  flash_sector_type_t (*get_sector_type)(const struct flash_iface *fif,
                                         int sector);

  error_t (*erase_sector)(const struct flash_iface *fif, int sector);

  error_t (*write)(const struct flash_iface *fif, int sector,
                   size_t offset, const void *data, size_t len);

  error_t (*compare)(const struct flash_iface *fif, int sector,
                     size_t offset, const void *data, size_t len);

  error_t (*read)(const struct flash_iface *fif, int sector,
                  size_t offset, void *data, size_t len);

  const void *(*get_addr)(const struct flash_iface *fif, int sector);

  // Primarily intended for in-place firmware upgrades
  // Code should reside in RAM only so we can overwrite running code
  void (*multi_write)(const struct flash_iface *fif,
                      const flash_multi_write_chunks_t *chunks,
                      const void *src_base,
                      int flags);

} flash_iface_t;

#define FLASH_MULTI_WRITE_CPU_REBOOT 0x1

const flash_iface_t *flash_get_primary(void);
