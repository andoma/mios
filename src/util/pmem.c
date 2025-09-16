#include <mios/pmem.h>
#include <sys/param.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN(x, y) (((x) + (y) - 1) & ~(y - 1))

static void
pmem_resize(pmem_t *p, size_t req)
{
  if(req <= p->capacity)
    return;

  size_t oldcap = p->capacity;
  p->capacity = MAX(req, 16 + p->capacity * 2);

  void *old = p->segments;
  p->segments = malloc(p->capacity * sizeof(pmem_segment_t));
  memcpy(p->segments, old, oldcap * sizeof(pmem_segment_t));
  free(old);
}


static pmem_segment_t *
pmem_insert(pmem_t *p, size_t pos)
{
  pmem_resize(p, p->count + 1);
  if(p->count - pos) {
    memmove(p->segments + pos + 1, p->segments + pos,
            sizeof(pmem_segment_t) * (p->count - pos));
  }
  p->count++;
  return p->segments + pos;
}


static void
pmem_erase(pmem_t *p, size_t pos)
{
  if(pos != p->count - 1) {
    memmove(p->segments + pos, p->segments + pos + 1,
            sizeof(pmem_segment_t) * (p->count - pos));
  }
  p->count--;
}


static void
pmem_merge(pmem_t *p, size_t pos)
{
  if(pos > 0 && p->segments[pos].type == p->segments[pos - 1].type) {
    p->segments[pos - 1].size += p->segments[pos].size;
    pmem_erase(p, pos);
    pos--;
  }

  if(pos < p->count - 1 &&
     p->segments[pos].type == p->segments[pos + 1].type) {
    p->segments[pos].size += p->segments[pos + 1].size;
    pmem_erase(p, pos + 1);
  }
}


void
pmem_add(pmem_t *p, unsigned long paddr, unsigned long size, uint32_t type)
{
  pmem_segment_t *ps = NULL;

  paddr = ALIGN(paddr, p->minimum_alignment);
  size = ALIGN(size, p->minimum_alignment);

  for(size_t i = 0; i < p->count; i++) {
    if(p->segments[i].paddr > paddr) {
      ps = pmem_insert(p, i);
      break;
    }
  }
  if(ps == NULL) {
    ps = pmem_insert(p, p->count);
  }

  ps->paddr = paddr;
  ps->size = size;
  ps->type = type;
  pmem_merge(p, ps - p->segments);
}



int
pmem_set(pmem_t *p, unsigned long paddr, unsigned long size, uint32_t type)
{
  pmem_segment_t *ps;

  paddr = ALIGN(paddr, p->minimum_alignment);
  size = ALIGN(size, p->minimum_alignment);

  for(size_t i = 0; i < p->count; i++) {
    if(paddr >= p->segments[i].paddr &&
       paddr + size <= p->segments[i].paddr + p->segments[i].size) {

      if(paddr == p->segments[i].paddr && size == p->segments[i].size) {
        if(p->segments[i].type == type)
          return 0;

        // Full replace
        p->segments[i].type = type;
        pmem_merge(p, i);
        return 0;
      }

      if(paddr == p->segments[i].paddr) {
        // At start
        p->segments[i].paddr += size;
        p->segments[i].size -= size;
        ps = pmem_insert(p, i);
        ps->paddr = paddr;
        ps->size = size;
        ps->type = type;
        pmem_merge(p, i);
        return 0;
      }

      if(paddr + size == p->segments[i].paddr + p->segments[i].size) {
        // At end
        p->segments[i].size -= size;
        ps = pmem_insert(p, i + 1);
        ps->paddr = paddr;
        ps->size = size;
        ps->type = type;
        pmem_merge(p, i + 1);
        return 0;
      }

      ps = pmem_insert(p, i + 1);
      ps->paddr = paddr;
      ps->size = size;
      ps->type = type;

      ps = pmem_insert(p, i + 2);
      ps->paddr = paddr + size;
      ps->size = p->segments[i].size - size - (paddr - p->segments[i].paddr);
      ps->type = p->segments[i].type;

      p->segments[i].size = paddr - p->segments[i].paddr;
      pmem_merge(p, i + 1);
      return 0;
    }

    if(paddr >= p->segments[i].paddr &&
       paddr < p->segments[i].paddr + p->segments[i].size) {

      long split = p->segments[i].paddr + p->segments[i].size;
      if(pmem_set(p, paddr, split - paddr, type))
        return -1;
      return pmem_set(p, split, size - (split - paddr), type);
    }
  }
  return -1;
}


unsigned long
pmem_alloc(pmem_t *p, unsigned long size, uint32_t from_type, uint32_t as_type,
           unsigned long align)
{
  if(p->count == 0 || align == 0)
    return 0;

  for(ssize_t i = p->count - 1; i >= 0; i--) {
    if(p->segments[i].type != from_type)
      continue;

    if(size > p->segments[i].size)
      continue;

    unsigned long paddr = (p->segments[i].paddr + (p->segments[i].size - size)) & ~align;

    if(pmem_set(p, paddr, size, as_type)) {
      continue;
    }
    return paddr;
  }
  return 0;
}
