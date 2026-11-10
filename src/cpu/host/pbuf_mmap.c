/*
 * Host debugging allocator for packet buffers: every buffer the pbuf
 * pools hand out is its own page, and returning it takes the page away.
 *
 * The pools proper recycle buffers at once, so a stale pointer reads or
 * scribbles on whoever got the buffer next and the damage shows up far
 * from the cause. Here it faults on the spot, with the guilty access in
 * the panic:
 *
 *   - use after free, and double free: the page is inaccessible
 *   - overrun: the buffer ends flush against an inaccessible page
 *   - use of uninitialised contents: a fresh buffer is filled with
 *     0xa5 rather than the zeroes a new page has, like a recycled
 *     buffer on a real target is full of old packets
 *
 * Unlike heap_mmap.c this does not mmap() and munmap() per buffer. The
 * kernel likes to hand a range that was just unmapped to the next
 * mapping of the same size, and with buffers all the same size and
 * recycled constantly, a stale pointer would then often hit a live
 * buffer again. Instead the buffers come out of one big reserved
 * range, front to back: an address is not used again until the range
 * wraps, millions of allocations later.
 *
 * The pools still count buffers, so running out behaves as on a real
 * target. Selected by ENABLE_PBUF_MMAP, which the host platform sets;
 * see pbuf_pool_take() and pbuf_pool_put() in net/pbuf.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <mios/mios.h>

#include "linux.h"

// 64 GB of address space (nothing is committed until used). Small
// enough for an aarch64 kernel with 39 bit addresses
#define PBUF_MMAP_REGION (1ull << 36)

static size_t g_pagesize;
static uint8_t *g_region;
static uint8_t *g_live;       // One byte per slot, nonzero while handed out
static size_t g_slots;
static size_t g_next;

// A slot is one page of data and one page that stays inaccessible
#define SLOT_SIZE (2 * g_pagesize)

static void
pbuf_mmap_init(void)
{
  // Smallest page size mprotect() accepts, as in heap_mmap.c
  size_t ps;
  for(ps = 4096; ps < 65536; ps *= 4) {
    void *p = linux_mmap(NULL, ps * 2, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(p == NULL)
      continue;
    const long r = linux_syscall(SYS_mprotect, (uint8_t *)p + ps, ps, 0);
    linux_syscall(SYS_munmap, p, ps * 2);
    if(r == 0)
      break;
  }
  g_pagesize = ps;
  g_slots = PBUF_MMAP_REGION / SLOT_SIZE;

  g_region = linux_mmap(NULL, PBUF_MMAP_REGION, 0,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  g_live = linux_mmap(NULL, g_slots, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if(g_region == NULL || g_live == NULL)
    panic("pbuf_mmap: cannot reserve address space");
}


// Both are called with interrupts off at IRQ_LEVEL_NET, like the pool
// functions they stand in for

void *
pbuf_mmap_get(size_t size)
{
  if(g_region == NULL)
    pbuf_mmap_init();

  if(size > g_pagesize)
    panic("pbuf_mmap: %zd byte buffers do not fit a page", size);

  // After a wrap, step over the few buffers that are still out
  while(g_live[g_next]) {
    if(++g_next == g_slots)
      g_next = 0;
  }
  const size_t slot = g_next;
  if(++g_next == g_slots)
    g_next = 0;

  uint8_t *page = g_region + slot * SLOT_SIZE;
  if(linux_syscall(SYS_mprotect, page, g_pagesize, PROT_READ | PROT_WRITE))
    panic("pbuf_mmap: mprotect failed");
  g_live[slot] = 1;

  // Flush against the inaccessible page above, then down to an
  // alignment that suits any buffer
  uint8_t *buf = page + g_pagesize - size;
  buf -= (uintptr_t)buf & 15;
  memset(buf, 0xa5, size);
  return buf;
}


void
pbuf_mmap_put(void *buf)
{
  const size_t off = (uint8_t *)buf - g_region;
  if(buf == NULL || (uint8_t *)buf < g_region || off >= PBUF_MMAP_REGION)
    panic("pbuf_mmap: %p is not a packet buffer", buf);

  const size_t slot = off / SLOT_SIZE;
  uint8_t *page = g_region + slot * SLOT_SIZE;

  // A second free, or a free of something inside the upper page,
  // faults here rather than upsetting the pool's count
  (void)*(volatile uint8_t *)buf;

  // Map a new inaccessible page over it. That gives the memory back,
  // which mprotect() would not, and merges with the inaccessible
  // neighbours so the number of mappings stays small
  if(linux_mmap(page, g_pagesize, 0,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                -1, 0) != page)
    panic("pbuf_mmap: remap failed");
  g_live[slot] = 0;
}
