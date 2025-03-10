#include <mios/block.h>
#include <mios/mios.h>
#include <mios/error.h>

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

typedef struct {
  block_iface_t iface;
  block_iface_t *parent;
  size_t offset;
} partition_t;

static error_t
partition_erase(struct block_iface *bi, size_t block)
{
  partition_t *p = (partition_t *)bi;
  if (block >= p->iface.num_blocks)
   return ERR_NOSPC;
  return p->parent->erase(p->parent, block + p->offset);
}

static error_t
partition_write(struct block_iface *bi, size_t block,
               size_t offset, const void *data, size_t length)
{
  partition_t *p = (partition_t *)bi;
  if (block >= p->iface.num_blocks)
    return ERR_NOSPC;

  if (offset + length > p->iface.block_size)
    return ERR_INVALID_LENGTH;

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


static error_t
partition_lock_erase(struct block_iface *bi, size_t block)
{
  partition_t *p = (partition_t *)bi;
  p->parent->ctrl(p->parent, BLOCK_LOCK);
  error_t err = partition_erase(bi, block);
  p->parent->ctrl(p->parent, BLOCK_UNLOCK);
  return err;
}

static error_t
partition_lock_write(struct block_iface *bi, size_t block,
               size_t offset, const void *data, size_t length)
{
  partition_t *p = (partition_t *)bi;
  p->parent->ctrl(p->parent, BLOCK_LOCK);
  error_t err = partition_write(bi, block, offset, data, length);
  p->parent->ctrl(p->parent, BLOCK_UNLOCK);
  return err;
}

static error_t
partition_lock_read(struct block_iface *bi, size_t block,
                    size_t offset, void *data, size_t length)
{
  partition_t *p = (partition_t *)bi;
  p->parent->ctrl(p->parent, BLOCK_LOCK);
  error_t err = partition_read(bi, block, offset, data, length);
  p->parent->ctrl(p->parent, BLOCK_UNLOCK);
  return err;
}

static error_t
partition_lock_ctrl(struct block_iface *bi, block_ctrl_op_t op)
{
  if(op == BLOCK_LOCK || op == BLOCK_UNLOCK)
    return 0;

  partition_t *p = (partition_t *)bi;
  p->parent->ctrl(p->parent, BLOCK_LOCK);
  error_t err = partition_ctrl(bi, op);
  p->parent->ctrl(p->parent, BLOCK_UNLOCK);
  return err;
}

block_iface_t *
block_create_partition(block_iface_t *parent,
                       size_t block_offset,
                       size_t num_blocks,
                       int flags)
{
  partition_t *p = malloc(sizeof(partition_t));

  p->iface.num_blocks = num_blocks;
  p->iface.block_size = parent->block_size;

  if(flags & BLOCK_PARTITION_AUTOLOCK) {
    p->iface.erase = partition_lock_erase;
    p->iface.write = partition_lock_write;
    p->iface.read = partition_lock_read;
    p->iface.ctrl = partition_lock_ctrl;
  } else {
    p->iface.erase = partition_erase;
    p->iface.write = partition_write;
    p->iface.read = partition_read;
    p->iface.ctrl = partition_ctrl;
  }

  p->parent = parent;
  p->offset = block_offset;
  return &p->iface;
}















typedef struct {
  block_iface_t iface;
  block_iface_t *parent;
  int flags;
} verifier_t;

static error_t
verifier_erase(struct block_iface *bi, size_t block)
{
  verifier_t *v = (verifier_t *)bi;
  return v->parent->erase(v->parent, block);
}

static error_t
verifier_write(struct block_iface *bi, size_t block,
               size_t offset, const void *data, size_t length)
{
  verifier_t *v = (verifier_t *)bi;

  if(v->flags & BLOCK_VERIFIER_DUMP)
    sthexdump(stdio, "WRITE", data, length, block * 4096 + offset);

  error_t err = v->parent->write(v->parent, block, offset, data, length);
  if(err) {
    if(v->flags & BLOCK_VERIFIER_PANIC_ON_ERR)
      panic("blockverifier: write failed %s", error_to_string(err));
    return err;
  }

  void *copy = xalloc(length, CACHE_LINE_SIZE, MEM_MAY_FAIL | MEM_TYPE_DMA);
  if(copy == NULL) {
    if(v->flags & BLOCK_VERIFIER_PANIC_ON_ERR)
      panic("blockverifier: no mem");
    return ERR_NO_MEMORY;
  }

  err = v->parent->read(v->parent, block, offset, copy, length);
  if(err) {
    if(v->flags & BLOCK_VERIFIER_PANIC_ON_ERR)
      panic("blockverifier: write readback failed %s", error_to_string(err));
    free(copy);
    return err;
  }

  if(memcmp(copy, data, length)) {
    sthexdump(stdio, "WRITE", data, length, block * 4096 + offset);
    sthexdump(stdio, "READB", copy, length, block * 4096 + offset);
    if(v->flags & BLOCK_VERIFIER_PANIC_ON_ERR)
      panic("blockverifier: write readback mismatch");
    free(copy);
    return err;
  }

  free(copy);
  return 0;
}

static error_t
verifier_read(struct block_iface *bi, size_t block,
               size_t offset, void *data, size_t length)
{
  verifier_t *v = (verifier_t *)bi;

  error_t err = v->parent->read(v->parent, block, offset, data, length);
  if(err) {
    if(v->flags & BLOCK_VERIFIER_PANIC_ON_ERR)
      panic("blockverifier: read failed %s", error_to_string(err));
    return err;
  }
  if(v->flags & BLOCK_VERIFIER_DUMP)
    sthexdump(stdio, "READ ", data, length, block * 4096 + offset);

  void *copy = xalloc(length, CACHE_LINE_SIZE, MEM_MAY_FAIL | MEM_TYPE_DMA);
  if(copy == NULL) {
    if(v->flags & BLOCK_VERIFIER_PANIC_ON_ERR)
      panic("blockverifier: no mem");
    return ERR_NO_MEMORY;
  }

  err = v->parent->read(v->parent, block, offset, copy, length);
  if(err) {
    if(v->flags & BLOCK_VERIFIER_PANIC_ON_ERR)
      panic("blockverifier: read2 failed %s", error_to_string(err));
    return err;
  }


  if(memcmp(copy, data, length)) {
    sthexdump(stdio, "READ1", data, length, block * 4096 + offset);
    sthexdump(stdio, "READ2", copy, length, block * 4096 + offset);
    if(v->flags & BLOCK_VERIFIER_PANIC_ON_ERR)
      panic("blockverifier: read mismatch");
    free(copy);
    return err;
  }
  free(copy);
  return 0;
}

static error_t
verifier_ctrl(struct block_iface *bi, block_ctrl_op_t op)
{
  verifier_t *v = (verifier_t *)bi;
  return v->parent->ctrl(v->parent, op);
}


block_iface_t *
block_create_verifier(block_iface_t *parent, int flags)
{
  verifier_t *v = malloc(sizeof(verifier_t));
  v->flags = flags;

  v->iface.num_blocks = parent->num_blocks;
  v->iface.block_size = parent->block_size;

  v->iface.erase = verifier_erase;
  v->iface.write = verifier_write;
  v->iface.read = verifier_read;
  v->iface.ctrl = verifier_ctrl;
  v->parent = parent;
  return &v->iface;
}
