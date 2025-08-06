#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct mios_image {
  uint8_t app_version[21];  // 20 bytes SHA1 + 1 byte dirty flag
  uint8_t mios_version[21]; // 20 bytes SHA1 + 1 byte dirty flag
  uint8_t buildid[20];      // 20 byte SHA1 from toolchain

  uint32_t buildid_paddr;   // Physical address of buildid on chip

  uint32_t load_addr;       // Where to start writing image to flash
  size_t image_size;        // Number of bytes in image[]
  uint8_t image[0];         // Actual bytes
} mios_image_t;


// mios_image_t should be free'd with standard libc free() (single allocation)
mios_image_t *mios_image_from_elf_mem(const void *elf, size_t elfsize);

// Loads from disk and calls mios_image_from_elf_mem()
// On failure NULL is returned and errno is set
mios_image_t *mios_image_from_elf_file(const char *path);
