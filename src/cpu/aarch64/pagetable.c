// EL1 stage-1 TTBR0 page table management. The root L1 table is set up
// in entry.S (one 1 GB block descriptor per 512 entries, identity-
// mapping the bottom 512 GB). This module lets the platform refine
// that mapping at 2 MB or 4 KB granule by promoting blocks to tables
// on demand.
//
//   pt_set / pt_unmap          - 4 KB-aligned range; uses 2 MB blocks
//                                where it can, 4 KB pages elsewhere
//   pt_set_gb / pt_unmap_gb    - 1 GB-granule shortcut, cheaper when
//                                the whole 1 GB region wants the same
//                                attrs
//   pt_apply                   - install promoted tables, flush
//                                descriptor caches, invalidate TLBs

#include "pagetable.h"

#include <stdio.h>

#include <mios/mios.h>

#include "cache.h"


// Block / page / table descriptor field bits.
//   Bits[1:0]:
//     L1/L2: 0b01 = Block, 0b11 = Table, else invalid
//     L3:    0b11 = Page, else invalid
// PT_TABLE is bit 1 — at L1/L2 it flips Block→Table, at L3 it's the
// "this is a valid page" indicator.
#define PT_VALID    (1ULL << 0)
#define PT_TABLE    (1ULL << 1)
#define PT_ATTR(i)  (((uint64_t)(i) & 7) << 2)
#define PT_NS       (1ULL << 5)
#define PT_AP_RW    (0ULL << 6)   // R/W at EL1, no access at EL0
#define PT_SH_ISH   (3ULL << 8)   // Inner Shareable
#define PT_AF       (1ULL << 10)  // Access Flag

// Common attributes for the descriptors we generate. Matches what
// entry.S puts in the initial L1 blocks (NS, EL1 R/W, Inner-Shareable,
// AF, nG=0) so a block→table promotion preserves observable attrs.
#define PT_BLOCK_COMMON (PT_VALID | PT_NS | PT_AP_RW | PT_SH_ISH | PT_AF)

#define GB              (1ULL << 30)
#define MB2             (2ULL << 20)
#define MB2_MASK        (MB2 - 1)
#define KB4             (1ULL << 12)
#define KB4_MASK        (KB4 - 1)

// Bits where an L2 block holds its output PA (bits[47:21]).
#define L2_BLOCK_PA_MASK 0x000FFFFFFFE00000ULL
// Bits where a Table or L3 page descriptor holds its next-level PA
// (bits[47:12]).
#define TABLE_PA_MASK    0x000FFFFFFFFFF000ULL


// Unified pool of 4 KB tables. Each is either an L2 (covering 1 GB at
// 2 MB granule) or an L3 (covering 2 MB at 4 KB granule); structurally
// identical so they share the pool. 64 × 4 KB = 256 KB static.
#define MAX_TABLES      64

static uint64_t  table_pool[MAX_TABLES][512] __attribute__((aligned(4096)));
static uint64_t  table_pas[MAX_TABLES];   // cached PA per table
static int       table_pool_used;

static int       gb_to_l2[512];           // GB → table_pool index (or -1)
static int       initialized;


static uint64_t
va_to_pa(const void *va)
{
  uint64_t par;
  __asm__ volatile("at s1e1r, %1; isb; mrs %0, par_el1"
                   : "=r"(par) : "r"(va));
  if(par & 1)
    return ~0ULL;
  return (par & TABLE_PA_MASK) | ((uintptr_t)va & KB4_MASK);
}


static uint64_t *
get_ttbr0(void)
{
  uint64_t *p;
  __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(p));
  return p;
}


static void
init_once(void)
{
  if(initialized)
    return;
  initialized = 1;
  for(int i = 0; i < 512; i++)
    gb_to_l2[i] = -1;
  table_pool_used = 0;
}


static int
alloc_table(void)
{
  if(table_pool_used >= MAX_TABLES)
    panic("pt: table pool exhausted");
  int idx = table_pool_used++;
  table_pas[idx] = va_to_pa(table_pool[idx]);
  return idx;
}


static int
table_idx_by_pa(uint64_t pa)
{
  for(int i = 0; i < table_pool_used; i++) {
    if(table_pas[i] == pa)
      return i;
  }
  panic("pt: unknown table PA 0x%lx", (unsigned long)pa);
}


static uint64_t
make_block(uint64_t pa, pt_mem_type_t type)
{
  return pa | PT_BLOCK_COMMON | PT_ATTR(type);
}


static uint64_t
make_page(uint64_t pa, pt_mem_type_t type)
{
  // L3 page descriptor: same attributes as a block, but bits[1:0] = 11
  // (PT_VALID | PT_TABLE) instead of 01.
  return make_block(pa, type) | PT_TABLE;
}


// Get or allocate an L2 table for the given GB index. If the L1 block
// at that GB was a valid mapping, clone its attributes to all 512 L2
// entries so the effective mapping is unchanged until subsequent
// pt_set / pt_unmap calls refine specific 2 MB regions.
static uint64_t *
promote_l2(uint64_t *ttbr0, int gb)
{
  if(gb_to_l2[gb] >= 0)
    return table_pool[gb_to_l2[gb]];

  int idx = alloc_table();
  gb_to_l2[gb] = idx;
  uint64_t *l2 = table_pool[idx];

  uint64_t l1 = ttbr0[gb];
  if((l1 & 0x3) == 0x1) {
    uint64_t attrs    = (l1 & 0xFFFULL) | (l1 & 0xFFFF000000000000ULL);
    uint64_t base_pa  = (uint64_t)gb << 30;
    for(int j = 0; j < 512; j++)
      l2[j] = (base_pa + ((uint64_t)j << 21)) | attrs;
  } else {
    for(int j = 0; j < 512; j++)
      l2[j] = 0;
  }
  return l2;
}


// Get or allocate an L3 table covering the 2 MB block addressed by
// l2[l2_idx] within the given GB. If the L2 entry was already a Table
// descriptor (a previous L3 promotion), find and return that L3.
// Returns NULL if the L2 entry was invalid (caller can skip — there's
// nothing to refine).
static uint64_t *
promote_l3(uint64_t *ttbr0, int gb, int l2_idx)
{
  uint64_t *l2 = promote_l2(ttbr0, gb);

  uint64_t entry = l2[l2_idx];
  if((entry & 0x1) == 0)
    return NULL;  // already invalid; nothing to refine

  if((entry & 0x3) == 0x3) {
    // Already a Table descriptor: locate the existing L3 in our pool.
    return table_pool[table_idx_by_pa(entry & TABLE_PA_MASK)];
  }

  // It's a 2 MB block: promote to L3.
  int idx = alloc_table();
  uint64_t *l3 = table_pool[idx];

  uint64_t attrs   = (entry & 0xFFFULL) | (entry & 0xFFFF000000000000ULL);
  attrs |= PT_TABLE;  // bit 1 = "valid page" at L3
  uint64_t base_pa = entry & L2_BLOCK_PA_MASK;
  for(int j = 0; j < 512; j++)
    l3[j] = (base_pa + ((uint64_t)j << 12)) | attrs;

  l2[l2_idx] = table_pas[idx] | PT_VALID | PT_TABLE;
  return l3;
}


static void
write_l2_block(uint64_t *ttbr0, uint64_t pa, int unmap, pt_mem_type_t type)
{
  int gb     = pa >> 30;
  int l2_idx = (pa >> 21) & 0x1FF;
  uint64_t *l2 = promote_l2(ttbr0, gb);

  if((l2[l2_idx] & 0x3) == 0x3) {
    // 2 MB block being written over a region previously promoted to L3.
    // We don't free L3 tables — simplest is to walk the L3 and overwrite
    // every page with what we'd put in the block. Cheap (512 entries).
    uint64_t *l3 = table_pool[table_idx_by_pa(l2[l2_idx] & TABLE_PA_MASK)];
    for(int j = 0; j < 512; j++)
      l3[j] = unmap ? 0 : make_page(pa + ((uint64_t)j << 12), type);
    return;
  }

  l2[l2_idx] = unmap ? 0 : make_block(pa, type);
}


static void
write_l3_page(uint64_t *ttbr0, uint64_t pa, int unmap, pt_mem_type_t type)
{
  int gb     = pa >> 30;
  int l2_idx = (pa >> 21) & 0x1FF;
  int l3_idx = (pa >> 12) & 0x1FF;
  uint64_t *l3 = promote_l3(ttbr0, gb, l2_idx);
  if(l3 == NULL)
    return;  // L2 entry was invalid; nothing to write

  l3[l3_idx] = unmap ? 0 : make_page(pa, type);
}


static void
modify_range(uint64_t va, size_t size, int unmap, pt_mem_type_t type)
{
  init_once();
  if(size == 0)
    return;

  if((va & KB4_MASK) || (size & KB4_MASK))
    panic("pt: range 0x%lx + 0x%zx not 4 KB aligned",
          (unsigned long)va, size);

  uint64_t end = va + size;
  uint64_t v2_start = (va + MB2_MASK) & ~MB2_MASK;
  uint64_t v2_end   = end & ~MB2_MASK;

  uint64_t *ttbr0 = get_ttbr0();

  if(v2_start >= v2_end) {
    // Entire range fits inside one 2 MB block; do it all at 4 KB granule.
    for(uint64_t pa = va; pa < end; pa += KB4)
      write_l3_page(ttbr0, pa, unmap, type);
    return;
  }

  // Leading partial 2 MB block (4 KB pages)
  for(uint64_t pa = va; pa < v2_start; pa += KB4)
    write_l3_page(ttbr0, pa, unmap, type);

  // Middle full 2 MB blocks
  for(uint64_t pa = v2_start; pa < v2_end; pa += MB2)
    write_l2_block(ttbr0, pa, unmap, type);

  // Trailing partial 2 MB block (4 KB pages)
  for(uint64_t pa = v2_end; pa < end; pa += KB4)
    write_l3_page(ttbr0, pa, unmap, type);
}


void
pt_set(uint64_t va, size_t size, pt_mem_type_t type)
{
  modify_range(va, size, 0, type);
}


void
pt_unmap(uint64_t va, size_t size)
{
  modify_range(va, size, 1, 0);
}


void
pt_set_gb(int gb, pt_mem_type_t type)
{
  init_once();
  if(gb < 0 || gb >= 512)
    return;

  uint64_t *ttbr0 = get_ttbr0();
  if(gb_to_l2[gb] >= 0) {
    // Already promoted to L2: rewrite each VALID entry's attrs while
    // preserving invalids. Lets a caller stage unmaps first and then
    // change memory type without re-mapping the holes they punched.
    uint64_t *l2 = table_pool[gb_to_l2[gb]];
    uint64_t base_pa = (uint64_t)gb << 30;
    for(int j = 0; j < 512; j++) {
      if((l2[j] & 0x3) == 0x3) {
        // L3 table: rewrite each valid 4 KB page, preserving invalids.
        uint64_t *l3 = table_pool[table_idx_by_pa(l2[j] & TABLE_PA_MASK)];
        for(int k = 0; k < 512; k++) {
          if(l3[k] & PT_VALID)
            l3[k] = make_page(base_pa + ((uint64_t)j << 21) +
                              ((uint64_t)k << 12), type);
        }
      } else if(l2[j] & PT_VALID) {
        l2[j] = make_block(base_pa + ((uint64_t)j << 21), type);
      }
    }
  } else {
    ttbr0[gb] = make_block((uint64_t)gb << 30, type);
  }
}


void
pt_unmap_gb(int gb)
{
  init_once();
  if(gb < 0 || gb >= 512)
    return;

  uint64_t *ttbr0 = get_ttbr0();
  if(gb_to_l2[gb] >= 0) {
    uint64_t *l2 = table_pool[gb_to_l2[gb]];
    for(int j = 0; j < 512; j++) {
      if((l2[j] & 0x3) == 0x3) {
        uint64_t *l3 = table_pool[table_idx_by_pa(l2[j] & TABLE_PA_MASK)];
        for(int k = 0; k < 512; k++)
          l3[k] = 0;
      } else {
        l2[j] = 0;
      }
    }
  } else {
    ttbr0[gb] = 0;
  }
}


void
pt_apply(void)
{
  uint64_t *ttbr0 = get_ttbr0();

  // Install promoted L2 tables: replace the original 1 GB L1 block
  // descriptors with table descriptors pointing at our L2 pages.
  for(int gb = 0; gb < 512; gb++) {
    if(gb_to_l2[gb] < 0)
      continue;
    ttbr0[gb] = table_pas[gb_to_l2[gb]] | PT_VALID | PT_TABLE;
  }

  // Make the descriptor changes visible to the table walker.
  __asm__ volatile("dsb sy; isb");
  for(int i = 0; i < table_pool_used; i++)
    cache_op(table_pool[i], sizeof(table_pool[i]), DCACHE_CLEAN_INV);
  cache_op(ttbr0, 512 * sizeof(uint64_t), DCACHE_CLEAN_INV);
  __asm__ volatile("dsb sy; isb");
  __asm__ volatile("tlbi vmalle1; dsb ish; isb" ::: "memory");
}
