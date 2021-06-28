#pragma once

#include "error.h"

typedef struct flash_iface {

  size_t (*get_sector_size)(const struct flash_iface *fif, int sector);

  error_t (*erase_sector)(const struct flash_iface *fif, int sector);

  error_t (*write)(const struct flash_iface *fif, int sector,
                   size_t offset, const void *data, size_t len);

  error_t (*compare)(const struct flash_iface *fif, int sector,
                     size_t offset, const void *data, size_t len);

  error_t (*read)(const struct flash_iface *fif, int sector,
                  size_t offset, void *data, size_t len);

} flash_iface_t;
