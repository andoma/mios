#pragma once

#include "error.h"

typedef enum {
  BLOCK_LOCK,
  BLOCK_UNLOCK,
  BLOCK_SYNC,
  BLOCK_SUSPEND,
  BLOCK_SHUTDOWN,
} block_ctrl_op_t;

typedef struct block_iface {

  size_t num_blocks;

  size_t block_size;

  error_t (*erase)(struct block_iface *bi, size_t block);

  error_t (*write)(struct block_iface *bi, size_t block,
                   size_t offset, const void *data, size_t length);

  error_t (*read)(struct block_iface *bi, size_t block,
                  size_t offset, void *data, size_t length);

  error_t (*ctrl)(struct block_iface *bi, block_ctrl_op_t op);

} block_iface_t;
