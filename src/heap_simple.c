#include <stdio.h>
#include <assert.h>
#include <sys/queue.h>
#include <stddef.h>
#include <string.h>
#include "heap.h"


#define ALIGN(a, b) (((a) + (b) - 1) & ~((b) - 1))

typedef struct heap_block {
  int hb_magic;
#define HEAP_MAGIC_FREE   0xf4eef4ee
#define HEAP_MAGIC_ALLOC   0xa110ced

  size_t hb_size;  // Size of block including this header struct
  TAILQ_ENTRY(heap_block) hb_link;
} heap_block_t;

TAILQ_HEAD(heap_block_queue, heap_block);

static struct heap_block_queue heap_blocks;


void
heap_init(void *start, size_t size)
{
  heap_block_t *hb = (void *)start;
  hb->hb_size = size;
  hb->hb_magic = HEAP_MAGIC_FREE;
  TAILQ_INIT(&heap_blocks);
  TAILQ_INSERT_TAIL(&heap_blocks, hb, hb_link);
}


void *
malloc(size_t size)
{
  heap_block_t *hb;

  size += sizeof(heap_block_t);
  size = ALIGN(size, 16);

  TAILQ_FOREACH(hb, &heap_blocks, hb_link) {
    if(hb->hb_magic != HEAP_MAGIC_FREE)
      continue;

    if(size <= hb->hb_size) {
      const size_t remain = hb->hb_size - size;
      if(remain < sizeof(heap_block_t) * 2) {
        size = hb->hb_size;
      } else {
        heap_block_t *split = (void *)hb + size;
        split->hb_magic = HEAP_MAGIC_FREE;
        split->hb_size = remain;
        TAILQ_INSERT_AFTER(&heap_blocks, hb, split, hb_link);
      }

      hb->hb_magic = HEAP_MAGIC_ALLOC;
      hb->hb_size = size;
      return (void *)(hb + 1);
    }
  }
  return NULL;
}


static void
heap_merge_next(heap_block_t *hb)
{
  heap_block_t *next = TAILQ_NEXT(hb, hb_link);
  if(next == NULL || next->hb_magic != HEAP_MAGIC_FREE)
    return;
  assert(next > hb);
  TAILQ_REMOVE(&heap_blocks, next, hb_link);
  hb->hb_size += next->hb_size;
}

void
free(void *ptr)
{
  if(ptr == NULL)
    return;
  heap_block_t *hb = ptr;
  hb--;
  assert(hb->hb_magic == HEAP_MAGIC_ALLOC);
  hb->hb_magic = HEAP_MAGIC_FREE;

  heap_merge_next(hb);
  heap_block_t *prev = TAILQ_PREV(hb, heap_block_queue, hb_link);
  if(prev != NULL) {
    assert(prev < hb);
    heap_merge_next(prev);
  }
}


static size_t
heap_usable_size(void *ptr)
{
  heap_block_t *hb = ptr;
  hb--;
  assert(hb->hb_magic == HEAP_MAGIC_ALLOC);
  return hb->hb_size - sizeof(heap_block_t);
}


void *
realloc(void *ptr, size_t size)
{
  void *n = NULL;
  if(size) {
    size_t cursize = heap_usable_size(ptr);
    if(size < cursize)
      return ptr;

    n = malloc(size);
    if(n == NULL)
      return NULL;

    if(ptr != NULL)
      memcpy(n, ptr, cursize);
  }
  free(ptr);
  return n;
}

void
heap_dump(void)
{
  heap_block_t *hb;
  printf(" --- Heap allocation dump ---\n");
  TAILQ_FOREACH(hb, &heap_blocks, hb_link) {
    printf("  %s 0x%x @ %p\n",
           hb->hb_magic == HEAP_MAGIC_ALLOC ? "use " :
           hb->hb_magic == HEAP_MAGIC_FREE  ? "free" :
           "????",
           hb->hb_size, hb);
  }
}

