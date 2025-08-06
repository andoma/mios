#pragma once

#include <stdint.h>
#include <stddef.h>

struct mios_image;

// Flash an image to stm32 with fdcan protocol.
// Chip must be in bootloader already
// Return -1 on failure, 0 on OK
// Fills errbuf with details about error

// Return values from rx and tx callbacks:
// OK: Return 0
// Error: Return negative error value (-errno)

int stm32_fdcan_flasher(const struct mios_image *image,
                        void *opaque,
                        int (*tx)(void *opaque,
                                  uint32_t can_id,
                                  const void *buf,
                                  size_t len),
                        int (*rx)(void *opaque,
                                  uint32_t can_id,
                                  void *buf,
                                  size_t len,
                                  int timeout),
                        void (*logmsg)(void *opaque,
                                       const char *msg),
                        int force_upgrade,
                        uint32_t can_id_offset,
                        char *errbuf,
                        size_t errlen);
