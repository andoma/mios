#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct mios_image {
  const char *appname;      // Points into image[]
  uint8_t app_version[21];  // 20 bytes SHA1 + 1 byte dirty flag
  uint8_t mios_version[21]; // 20 bytes SHA1 + 1 byte dirty flag
  uint8_t buildid[20];      // 20 byte SHA1 from toolchain
  uint64_t buildid_paddr;   // Physical address of buildid on chip

  uint64_t load_addr;       // Where to start writing image to flash
  size_t image_size;        // Number of bytes in image[]
  uint8_t image[0];         // Actual bytes
} mios_image_t;


/**
 * mios_image_t should be free'd with standard libc free() (single
 * allocation)
 *
 * @skip_bytes: is number of bytes from the lowest address that will be
 * skipped in the output image. This is typically used to skip over
 * built-in bootloaders, etc
 *
 * @alignment: pads the image_size to be a multiple of the alignemnt
 * (does not need to be a power-of-two). Padding will be filled with
 * 0xff.
 *
 *
 */

mios_image_t *mios_image_from_elf_mem(const void *elf, size_t elfsize,
                                      size_t skip_bytes, size_t alignment,
                                      const char **errmsg);

/**
 * Helper to load ELF image from file system.
 *
 * For other arguments, please see @mios_image_from_elf_mem
 */

mios_image_t *mios_image_from_elf_file(const char *path, size_t skip_bytes,
                                       size_t alignment, const char **errmsg);

