#include <mios/block.h>

#include <stdlib.h>

typedef struct {
  block_iface_t iface;
  block_iface_t *parent;
  size_t offset;
} partition_t;

static error_t
partition_erase(struct block_iface *bi, size_t block)
{
  partition_t *p = (partition_t *)bi;
  return p->parent->erase(p->parent, block + p->offset);
}

static error_t
partition_write(struct block_iface *bi, size_t block,
               size_t offset, const void *data, size_t length)
{
  partition_t *p = (partition_t *)bi;
  return p->parent->write(p->parent, block + p->offset, offset, data, length);
}

static error_t
partition_read(struct block_iface *bi, size_t block,
               size_t offset, void *data, size_t length)
{
  partition_t *p = (partition_t *)bi;
  return p->parent->read(p->parent, block + p->offset, offset, data, length);
}

static error_t
partition_ctrl(struct block_iface *bi, block_ctrl_op_t op)
{
  partition_t *p = (partition_t *)bi;
  return p->parent->ctrl(p->parent, op);
}

block_iface_t *
block_create_partition(block_iface_t *parent,
                       size_t block_offset,
                       size_t num_blocks)
{
  partition_t *p = malloc(sizeof(partition_t));

  p->iface.num_blocks = num_blocks;
  p->iface.block_size = parent->block_size;

  p->iface.erase = partition_erase;
  p->iface.write = partition_write;
  p->iface.read = partition_read;
  p->iface.ctrl = partition_ctrl;

  p->parent = parent;
  p->offset = block_offset;
  return &p->iface;
}
