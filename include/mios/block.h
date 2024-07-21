#pragma once

#include <stddef.h>

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

#define BLOCK_PARTITION_AUTOLOCK 0x1

block_iface_t *block_create_partition(block_iface_t *parent,
                                      size_t block_offset,
                                      size_t num_blocks,
                                      int partition_flags);

#define BLOCK_VERIFIER_PANIC_ON_ERR 0x1
#define BLOCK_VERIFIER_DUMP         0x2

block_iface_t *block_create_verifier(block_iface_t *parent, int flags);
