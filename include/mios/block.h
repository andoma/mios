#pragma once

#include "error.h"

typedef struct block_iface {

  size_t num_blocks;

  size_t block_size;

  error_t (*erase)(struct block_iface *bi, size_t block);

  error_t (*write)(struct block_iface *bi, size_t block,
                   size_t offset, const void *data, size_t length);

  error_t (*read)(struct block_iface *bi, size_t block,
                  size_t offset, void *data, size_t length);

  error_t (*sync)(struct block_iface *bi);

  error_t (*suspend)(struct block_iface *bi);

} block_iface_t;
