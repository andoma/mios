#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <sys/queue.h>
#include <stddef.h>
#include <string.h>
#include <malloc.h>

#include <mios/mios.h>
#include <mios/cli.h>
#include <mios/task.h>

#define ALIGN(a, b) (((a) + (b) - 1) & ~((b) - 1))

extern unsigned long _ebss;
static const unsigned long ebss_start = (long)&_ebss;

static mutex_t heap_mutex = MUTEX_INITIALIZER("heap");

SLIST_HEAD(heap_header_slist, heap_header);

typedef struct heap_block {
  struct heap_block *next;
  uint32_t prev : 31;
  uint32_t free : 1;
} heap_block_t;


static struct heap_header_slist heaps;

typedef struct heap_header {
  heap_block_t *blocks;
  SLIST_ENTRY(heap_header) link;
  int type;
} heap_header_t;


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



void
heap_add_mem(long start, long end, int type)
{
  if(start == HEAP_START_EBSS) {
    start = ebss_start;
  }

  heap_header_t *hh = (void *)start;

  heap_block_t *hb = (void *)ALIGN((intptr_t)(hh + 1), sizeof(heap_block_t));

  heap_block_t *sentinel = (void *)ALIGN(end - sizeof(heap_block_t),
                                         sizeof(heap_block_t));

  hb->next = sentinel;
  hb->prev = 0;
  hb->free = 1;

  sentinel->next = NULL;
  hb_set_prev(sentinel, hb);
  sentinel->free = 0;

  hh->blocks = hb;
  hh->type = type;
  mutex_lock(&heap_mutex);
  SLIST_INSERT_HEAD(&heaps, hh, link);
  mutex_unlock(&heap_mutex);
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



#if 0
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
#endif




static error_t
cmd_mem(cli_t *cli, int argc, char **argv)
{
  mutex_lock(&heap_mutex);

  heap_header_t *hh;
  SLIST_FOREACH(hh, &heaps, link) {
    cli_printf(cli, "Heap at %p type 0x%x\n", hh, hh->type);
    heap_block_t *hb = hh->blocks;
    size_t use = 0;
    size_t avail = 0;
    while(hb->next) {
      cli_printf(cli, "\t%s @ %p size:0x%08x %d\n",
                 hb->free ? "free" : "used",
                 hb, hb_size(hb), hb_size(hb));
      if(hb->free)
        avail += hb_size(hb);
      else
        use += hb_size(hb);
      hb = hb->next;
    }
    cli_printf(cli, "\t%d bytes used, %d bytes free\n\n",
               use, avail);
  }
  mutex_unlock(&heap_mutex);
  return 0;
}

CLI_CMD_DEF("mem", cmd_mem);






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


static void *
malloc0(size_t size, size_t align, int type)
{
  mutex_lock(&heap_mutex);

  int heap_type = type & 0xf;

  heap_header_t *hh;
  SLIST_FOREACH(hh, &heaps, link) {
    heap_block_t *hb = hh->blocks;
    if(hb != NULL && (heap_type == 0 || heap_type == hh->type)) {
      void *x = heap_alloc(hb, size, align);
      if(x) {
        mutex_unlock(&heap_mutex);
        return x;
      }
    }
  }
  mutex_unlock(&heap_mutex);
  if(!(type & MEM_MAY_FAIL))
    panic("Out of memory (s=%d a=%d t=%d)", size, align, type & 0xf);
  return NULL;
}




void *
malloc(size_t size)
{
  return malloc0(size, 0, 0);
}

void *
calloc(size_t nmemb, size_t size)
{
  size *= nmemb;
  void *x = malloc0(size, 0, 0);
  memset(x, 0, size);
  return x;
}

void
free(void *ptr)
{
  mutex_lock(&heap_mutex);
  heap_free(ptr);
  mutex_unlock(&heap_mutex);
}


int
free_try(void *ptr)
{
  if(mutex_trylock(&heap_mutex))
    return 1;
  heap_free(ptr);
  mutex_unlock(&heap_mutex);
  return 0;
}


void *
memalign(size_t size, size_t alignment)
{
  return malloc0(size, alignment, 0);
}

void *
xalloc(size_t size, size_t alignment, unsigned int type)
{
  return malloc0(size, alignment, type);
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
