#include <mios/block.h>
#include <mios/mios.h>
#include <mios/error.h>

#include <sys/param.h>

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
partition_erase(struct block_iface *bi, size_t block, size_t count)
{
  partition_t *p = (partition_t *)bi;
  block += p->offset;
  if (block + count > p->iface.num_blocks)
    return ERR_NOSPC;
  return p->parent->erase(p->parent, block, count);
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
partition_lock_erase(struct block_iface *bi, size_t block, size_t count)
{
  partition_t *p = (partition_t *)bi;
  p->parent->ctrl(p->parent, BLOCK_LOCK);
  error_t err = partition_erase(bi, block, count);
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
verifier_erase(struct block_iface *bi, size_t block, size_t count)
{
  verifier_t *v = (verifier_t *)bi;
  return v->parent->erase(v->parent, block, count);
}

static error_t
verifier_write(struct block_iface *bi, size_t block,
               size_t offset, const void *data, size_t length)
{
  verifier_t *v = (verifier_t *)bi;

  const size_t addr = block * bi->block_size + offset;

  if(v->flags & BLOCK_VERIFIER_DUMP)
    sthexdump(stdio, "WRITE", data, length, addr);

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
    sthexdump(stdio, "WRITE", data, length, addr);
    sthexdump(stdio, "READB", copy, length, addr);
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

  const size_t addr = block * bi->block_size + offset;

  error_t err = v->parent->read(v->parent, block, offset, data, length);
  if(err) {
    if(v->flags & BLOCK_VERIFIER_PANIC_ON_ERR)
      panic("blockverifier: read failed %s", error_to_string(err));
    return err;
  }
  if(v->flags & BLOCK_VERIFIER_DUMP)
    sthexdump(stdio, "READ ", data, length, addr);

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
    sthexdump(stdio, "READ1", data, length, addr);
    sthexdump(stdio, "READ2", copy, length, addr);
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



typedef struct subdivision {
  block_iface_t iface;
  block_iface_t *parent;
  int shift;
} subdivision_t;

static size_t
sub_block_offset(subdivision_t *s, size_t block)
{
  return (((1 << s->shift) - 1) & block) * s->iface.block_size;
}

static error_t
subdivision_erase(struct block_iface *bi, size_t block, size_t count)
{
  error_t err;
  subdivision_t *s = (subdivision_t *)bi;
  const size_t mask = (1 << s->shift) - 1;


  while(count) {

    if(!(block & mask) && count > mask) {
      // Can do full erase downstream
      const size_t chunk = count & ~mask;
      err = block_erase(s->parent, block >> s->shift, count >> s->shift);
      if(err)
        return err;
      count -= chunk;
      block += chunk;
      continue;
    }

    const size_t pbs = s->parent->block_size;
    void *buf = xalloc(pbs, 0, MEM_MAY_FAIL);

    err = block_read(s->parent, block >> s->shift, 0, buf, pbs);
    if(!err) {
      err = block_erase(s->parent, block >> s->shift, 1);

      size_t offset = block & mask;
      size_t chunk = MIN(count, (1 << s->shift) - offset);

      memset(buf + offset * s->iface.block_size, 0xff,
             chunk * s->iface.block_size);

      if(!err) {
        err = block_write(s->parent, block >> s->shift, 0, buf, pbs);
      }

      count -= chunk;
      block += chunk;
    }

    free(buf);

    if(err)
      return err;
  }
  return 0;
}

static error_t
subdivision_read(struct block_iface *bi, size_t block,
                 size_t offset, void *data, size_t length)
{
  subdivision_t *s = (subdivision_t *)bi;
  return block_read(s->parent, block >> s->shift,
                    offset + sub_block_offset(s, block), data, length);
}

static error_t
subdivision_write(struct block_iface *bi, size_t block,
                  size_t offset, const void *data, size_t length)
{
  subdivision_t *s = (subdivision_t *)bi;
  return block_write(s->parent, block >> s->shift,
                     offset + sub_block_offset(s, block), data, length);
}

static error_t
subdivision_ctrl(struct block_iface *bi, block_ctrl_op_t op)
{
  subdivision_t *s = (subdivision_t *)bi;
  return block_ctrl(s->parent, op);
}

block_iface_t *
block_create_subdivision(block_iface_t *parent, int shift)
{
  subdivision_t *s = malloc(sizeof(subdivision_t));
  s->shift = shift;

  s->iface.num_blocks = parent->num_blocks << shift;
  s->iface.block_size = parent->block_size >> shift;

  s->iface.erase = subdivision_erase;
  s->iface.write = subdivision_write;
  s->iface.read = subdivision_read;
  s->iface.ctrl = subdivision_ctrl;
  s->parent = parent;
  return &s->iface;
}
