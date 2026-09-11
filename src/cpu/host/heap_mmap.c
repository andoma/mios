/*
 * Host debugging allocator: one mmap() per allocation, munmap() on free.
 *
 * Nothing is ever reused and nothing is adjacent, so the failure modes
 * that quietly corrupt a shared heap all turn into an immediate fault
 * with the guilty access in the panic:
 *
 *   - use after free, and double free: the pages are gone, so the
 *     access lands in a hole
 *   - overrun: the payload ends flush against a PROT_NONE guard page
 *
 * The payload is placed flush against the top because overrunning is
 * the common mistake; an underrun has to get past the header and the
 * alignment padding before it reaches the guard page below, so a small
 * one still goes unnoticed.
 *
 * It costs two syscalls per allocation and two guard pages of address
 * space per live object, which is nothing for a test process on a 64 bit
 * host and would be absurd on anything with real memory. Selected by
 * ENABLE_HEAP_MMAP, which the host platforms set; every other target
 * uses lib/libc/heap_simple.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>

#include <mios/mios.h>
#include <mios/cli.h>

#include "linux.h"

#define HEAP_MMAP_MAGIC 0x6d696f73686d61ull  // "mioshma"

// Sits immediately below the payload, inside the mapping
typedef struct alloc_header {
  uint64_t magic;
  void *base;        // start of the mapping, the lower guard page
  size_t maplen;     // its full length, both guard pages included
  size_t size;       // what the caller asked for
} alloc_header_t;

static size_t g_pagesize;

static size_t g_live_bytes;
static size_t g_live_count;
static size_t g_total_count;
static size_t g_retained_bytes;
static size_t g_retained_count;


/* mprotect() operates on whole pages, so the shortest length it accepts
   is the page size. Probing beats guessing: an aarch64 kernel may be
   built for 4k, 16k or 64k pages, and we have no getauxval() (nor, in
   library mode, an auxv to read it out of). */
static size_t
probe_pagesize(void)
{
  for(size_t ps = 4096; ps < 65536; ps *= 4) {
    void *p = linux_mmap(NULL, ps * 2, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(p == NULL)
      continue;
    const long r = linux_syscall(SYS_mprotect, (uint8_t *)p + ps, ps, 0);
    linux_syscall(SYS_munmap, p, ps * 2);
    if(r == 0)
      return ps;
  }
  return 65536;
}


// Every region is the mapping the allocation lives in, so there is
// nothing to add. The platform's own heap mmap is unused (see
// host_map_heap(), which the host platforms skip when we are in play).
void
heap_add_mem(long start, long end, uint8_t type, uint8_t prio)
{
}


static void *
malloc0(size_t size, size_t align, int type)
{
  if(g_pagesize == 0)
    g_pagesize = probe_pagesize();

  const size_t ps = g_pagesize;

  if(align < 16)
    align = 16;   // CPU_STACK_ALIGNMENT, and enough for alloc_header_t

  // Round the payload area up to whole pages, with room to push the
  // payload down to the requested alignment and still fit the header.
  const size_t need = size + sizeof(alloc_header_t) + align - 1;
  const size_t datalen = (need + ps - 1) & ~(ps - 1);
  const size_t maplen = datalen + 2 * ps;   // a guard page either side

  uint8_t *base = linux_mmap(NULL, maplen, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(base == NULL) {
    if(!(type & MEM_MAY_FAIL))
      panic("Out of memory (s=%zd a=%zd t=%d)", size, align, type & 0xf);
    return NULL;
  }

  linux_syscall(SYS_mprotect, base, ps, 0);
  linux_syscall(SYS_mprotect, base + ps + datalen, ps, 0);

  // Flush against the upper guard page, then down to the alignment
  uint8_t *payload = base + ps + datalen - size;
  payload -= (uintptr_t)payload & (align - 1);

  alloc_header_t *ah = (alloc_header_t *)payload - 1;
  ah->magic = HEAP_MMAP_MAGIC;
  ah->base = base;
  ah->maplen = maplen;
  ah->size = size;

  __atomic_add_fetch(&g_live_bytes, size, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_live_count, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_total_count, 1, __ATOMIC_RELAXED);

  // Fresh anonymous pages are already zero, so MEM_CLEAR is free
  return payload;
}


void *
malloc(size_t size)
{
  return malloc0(size, 0, 0);
}

void *
calloc(size_t nmemb, size_t size)
{
  return malloc0(nmemb * size, 0, MEM_CLEAR);
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


// Claim an allocation: validate it and drop it from the live counts.
// A second free reads a header that is no longer mapped and faults
// before it gets here; a pointer that was never ours usually gets this
// far and fails the magic.
static alloc_header_t *
claim(void *ptr, const char *what)
{
  alloc_header_t *ah = (alloc_header_t *)ptr - 1;
  if(ah->magic != HEAP_MMAP_MAGIC)
    panic("%s(%p): not the start of a live allocation (magic 0x%016lx)",
          what, ptr, ah->magic);
  ah->magic = 0;
  __atomic_sub_fetch(&g_live_bytes, ah->size, __ATOMIC_RELAXED);
  __atomic_sub_fetch(&g_live_count, 1, __ATOMIC_RELAXED);
  return ah;
}


void
free(void *ptr)
{
  if(ptr == NULL)
    return;

  const alloc_header_t *ah = claim(ptr, "free");
  linux_syscall(SYS_munmap, ah->base, ah->maplen);
}


// The kernel's only free_try() caller is thread_exit2(), which frees a
// dead thread's stack from a lightweight task that task_switch() is
// running *on that very stack* (see kernel/task.c). An allocator that
// just marks the block free can do that; unmapping the ground you are
// standing on cannot. So keep the mapping and let it leak: a test
// process can spare a stack per exited thread, and a stack is not where
// the interesting use-after-free lives.
int
free_try(void *ptr)
{
  if(ptr == NULL)
    return 0;

  const alloc_header_t *ah = claim(ptr, "free_try");
  __atomic_add_fetch(&g_retained_bytes, ah->size, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_retained_count, 1, __ATOMIC_RELAXED);
  return 0;
}


static error_t
cmd_show_malloc(cli_t *cli, int argc, char **argv)
{
  cli_printf(cli, "mmap allocator (ENABLE_HEAP_MMAP), page size %zd\n",
             g_pagesize);
  cli_printf(cli, "\t%zd allocations live, %zd bytes\n",
             __atomic_load_n(&g_live_count, __ATOMIC_RELAXED),
             __atomic_load_n(&g_live_bytes, __ATOMIC_RELAXED));
  cli_printf(cli, "\t%zd allocations since boot\n",
             __atomic_load_n(&g_total_count, __ATOMIC_RELAXED));
  cli_printf(cli, "\t%zd retained by free_try(), %zd bytes\n",
             __atomic_load_n(&g_retained_count, __ATOMIC_RELAXED),
             __atomic_load_n(&g_retained_bytes, __ATOMIC_RELAXED));
  return 0;
}

CLI_CMD_DEF_EXT("show_malloc", cmd_show_malloc, NULL, "Memory allocator info");
