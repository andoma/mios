#pragma once

#include <stdint.h>

#define EFI_VERSION 0x00010000
#define EFI_MAGIC   "EFI PART"
#define EFI_NAMELEN 36

struct efi_header {
  uint8_t magic[8];

  uint32_t version;
  uint32_t header_sz;

  uint32_t crc32;
  uint32_t reserved;

  uint64_t header_lba;
  uint64_t backup_lba;
  uint64_t first_lba;
  uint64_t last_lba;

  uint8_t volume_uuid[16];

  uint64_t entries_lba;

  uint32_t entries_count;
  uint32_t entries_size;
  uint32_t entries_crc32;
} __attribute__((packed));


struct efi_entry {
  uint8_t type_uuid[16];
  uint8_t uniq_uuid[16];
  uint64_t first_lba;
  uint64_t last_lba;
  uint64_t attr;
  uint16_t name[EFI_NAMELEN];
};
