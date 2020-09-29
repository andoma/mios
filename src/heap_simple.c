#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <sys/queue.h>
#include <stddef.h>
#include <string.h>
#include <mios.h>

#include <irq.h>

#include "heap.h"
#include "platform/platform.h"


#define ALIGN(a, b) (((a) + (b) - 1) & ~((b) - 1))


typedef struct heap_block {
  struct heap_block *next;
  uint32_t prev : 31;
  uint32_t free : 1;
} heap_block_t;

static heap_block_t *main_heap;


static inline heap_block_t *
hb_get_prev(const heap_block_t *hb)
{
  return (heap_block_t *)(hb->prev << 1);
}


static inline void
hb_set_prev(heap_block_t *hb, heap_block_t *prev)
{
  hb->prev = (intptr_t)prev >> 1;
}

static inline size_t
hb_size(const heap_block_t *hb)
{
  return (void *)hb->next - (void *)hb;
}

//static void malloc_tests(void);
//static void malloc_tests2(void);

static heap_block_t *
heap_create(void *start, void *end)
{
  heap_block_t *hb = (void *)ALIGN((intptr_t)start, sizeof(heap_block_t));

  heap_block_t *sentinel = (void *)ALIGN((intptr_t)end - sizeof(heap_block_t),
                                         sizeof(heap_block_t));

  hb->next = sentinel;
  hb->prev = 0;
  hb->free = 1;

  sentinel->next = NULL;
  hb_set_prev(sentinel, hb);
  sentinel->free = 0;

  return hb;
}



static void __attribute__((unused))
verify_heap(heap_block_t *hb)
{
  heap_block_t *prev = NULL;

  while(hb->next) {

    if(hb->free) {
      if(hb->next->free) {
        panic("Adjacent free blocks");
      }
    }
    if(hb_get_prev(hb) != prev) {
      panic("prevlink mismatch %p.prev(%p) != %p\n",
            hb, hb_get_prev(hb), prev);
    }
    prev = hb;
    hb = hb->next;
  }
  if(hb_get_prev(hb) != prev) {
    panic("sentinel prevlink failed");
  }
  assert(!hb->free); // Sentinel is never free
}




static void
heap_dump0(heap_block_t *hb)
{
  printf(" --- Heap allocation dump ---\n");

  heap_block_t *prev = NULL;

  while(hb->next) {
    printf("%s @ %p size:0x%08x prev=%p %s\n",
           hb->free ? "free" : "used",
           hb, hb_size(hb),
           hb_get_prev(hb),
           hb_get_prev(hb) == prev ? "OK" : "BAD PREVLINK");
    prev = hb;
    hb = hb->next;
  }
  printf("Sentinel @ %p             prev=%p %s\n", hb,
         hb_get_prev(hb),
         hb_get_prev(hb) == prev ? "OK" : "BAD PREVLINK");
}


void
heap_dump(void)
{
  heap_dump0(main_heap);
}

static void  __attribute__((constructor(120)))
heap_init(void)
{
  extern unsigned long _edata;
  extern unsigned long _ebss;
  void *heap_start = (void *)&_ebss;
  void *heap_end =   platform_heap_end();

  printf("\n\n\nRAM Layout edata:%p, ebss:%p, eheap:%p\n",
         &_edata, &_ebss, heap_end);

  main_heap = heap_create(heap_start, heap_end);
}


static void *
heap_alloc(heap_block_t *hb, size_t size, size_t align)
{
  if(align < sizeof(heap_block_t))
    align = sizeof(heap_block_t);

  size = ALIGN(size, sizeof(heap_block_t));

  for(; hb->next; hb = hb->next) {
    if(!hb->free)
      continue;

    const uint32_t addr = (intptr_t)(hb + 1);
    const uint32_t aligned_addr = ALIGN(addr, align);
    const uint32_t end = aligned_addr + size;

    if((intptr_t)end > (intptr_t)hb->next)
      continue; // Doesn't fit

    const uint32_t align_gap = aligned_addr - addr;
    if(align_gap) {
      // Need to pad before memory block
      assert(align_gap >= sizeof(heap_block_t));

      heap_block_t *n = hb->next;
      heap_block_t *p = hb_get_prev(hb);
      heap_block_t *split = hb;
      hb = (heap_block_t *)aligned_addr - 1;
      split->free = 1;

      if(p)
        p->next = split;

      hb_set_prev(split, p);
      split->next = hb;

      hb_set_prev(hb, split);
      hb->next = n;

      hb_set_prev(n, hb);
    }

    hb->free = 0;

    const uint32_t tail_gap = (intptr_t)hb->next - end;

    if(tail_gap > sizeof(heap_block_t)) {
      heap_block_t *n = hb->next;
      heap_block_t *split = (void *)(aligned_addr + size);
      split->free = 1;
      hb->next = split;
      split->next = n;
      hb_set_prev(split, hb);
      hb_set_prev(n, split);
    }

    return (void *)aligned_addr;
  }
  return NULL;
}


static void
heap_merge_next(heap_block_t *hb)
{
  heap_block_t *next = hb->next;
  if(!next->free)
    return;
  assert(next > hb);
  hb->next = next->next;
  hb_set_prev(next->next, hb);
}


static void
heap_free(void *ptr)
{
  if(ptr == NULL)
    return;

  heap_block_t *hb = (heap_block_t *)ptr - 1;
  assert(!hb->free);
  hb->free = 1;

  heap_merge_next(hb);
  heap_block_t *p = hb_get_prev(hb);
  if(p != NULL && p->free) {
    assert(p < hb);
    heap_merge_next(p);
  }

}



void *
malloc(size_t size)
{
  int s = irq_forbid(IRQ_LEVEL_SWITCH);
  void *x = heap_alloc(main_heap, size, 0);
  irq_permit(s);
  if(x == NULL)
    panic("Out of memory");
  return x;
}


void *
calloc(size_t nmemb, size_t size)
{
  size *= nmemb;
  void *x = malloc(size);
  if(x)
    memset(x, 0, size);
  return x;
}


void
free(void *ptr)
{
  int s = irq_forbid(IRQ_LEVEL_SWITCH);
  heap_free(ptr);
  irq_permit(s);
}


void *
memalign(size_t size, size_t alignment)
{
  int s = irq_forbid(IRQ_LEVEL_SWITCH);
  void *x = heap_alloc(main_heap, size, alignment);
  irq_permit(s);
  if(x == NULL)
    panic("Out of memory");
  return x;
}



#if 0

static void
malloc_tests(void)
{
  void *a = heap_alloc(main_heap, 256, 64);
  printf("a: malloc 256 @ 64 alignment = %p\n", a);
  heap_dump();

  void *b = heap_alloc(main_heap, 256, 64);
  printf("b: malloc 256 @ 64 alignment = %p\n", b);
  heap_dump();

  void *c = heap_alloc(main_heap, 256, 64);
  printf("c: malloc 256 @ 64 alignment = %p\n", c);
  heap_dump();


  free(b); b = NULL;
  printf("free(b)\n");
  heap_dump();

  void *d = heap_alloc(main_heap, 256, 64);
  printf("d: malloc 256 @ 64 alignment = %p\n", d);
  heap_dump();

  void *e = heap_alloc(main_heap, 0x34 - 12, 0);
  printf("e: malloc %d @ 64 alignment = %p\n", 0x34 - 12, e);
  heap_dump();


  free(a); a = NULL;
  printf("free(a)\n");
  heap_dump();
  free(c); c = NULL;
  printf("free(c)\n");
  heap_dump();

  void *f = heap_alloc(main_heap, 256, 64);
  printf("f: malloc 256 @ 64 alignment = %p\n", f);
  heap_dump();

  free(f); f = NULL;
  printf("free(f)\n");
  heap_dump();

  void *g = heap_alloc(main_heap, 256, 64);
  printf("g: malloc 256 @ 64 alignment = %p\n", g);
  heap_dump();

  free(e); e = NULL;
  printf("free(e)\n");
  heap_dump();

  void *h = heap_alloc(main_heap, 256, 64);
  printf("h: malloc 256 @ 64 alignment = %p\n", h);
  heap_dump();

  void *i = heap_alloc(main_heap, 4096, 4096);
  printf("i: malloc 4096 @ 64 alignment = %p\n", i);
  heap_dump();


  free(a);
  free(b);
  free(c);
  free(d);
  free(e);
  free(f);
  free(g);
  free(h);
  free(i);

  heap_dump();

  panic("stop");
}


static void
malloc_tests2(void)
{
#define ps 16
  void *p[ps] = {};

  int alignment = 1;
  int ao = 0;
  int fo = 0;
  int size = 1;
  while(1) {

    verify_heap(main_heap);

    if(p[ao & (ps - 1)] == NULL) {
      const size_t s = size  & 511;
      const size_t a = 1 << (alignment & 0x7);
      void * x = heap_alloc(main_heap, s, a);
      assert(x);
      assert(((intptr_t)x & (a - 1)) == 0);
      p[ao & (ps - 1)] = x;
    }

    verify_heap(main_heap);

    if(p[fo & (ps - 1)] != NULL) {

      void *ptr = p[fo & (ps - 1)];
      free(ptr);
      p[fo & (ps - 1)] = NULL;
    }

    size++;
    alignment++;
    ao += 1;
    fo += 7;
  }
}

#endif
