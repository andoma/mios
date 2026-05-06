#pragma once

#include <stddef.h>
#include <stdint.h>

// Memory type for stage-1 EL1 mappings. Index into MAIR_EL1, which is
// programmed in entry.S to:
//   slot 0: Device-nGnRnE
//   slot 1: Normal Inner+Outer Write-Back cacheable
//   slot 2: Normal Inner+Outer Non-Cacheable
typedef enum {
  PT_DEVICE_NGNRNE = 0,
  PT_NORMAL_WB     = 1,
  PT_NORMAL_NC     = 2,
} pt_mem_type_t;


// Identity-map a VA range with the given memory type. Both va and
// size must be 4 KB aligned; panics otherwise. Internally the range
// is split into a leading 4 KB-page span, a middle 2 MB-block span
// and a trailing 4 KB-page span; L2 tables are promoted to L3 only
// where the range edges fall mid-block.
void pt_set(uint64_t va, size_t size, pt_mem_type_t type);

// Mark a VA range as unmapped (translation faults on access). Same
// 4 KB granule + alignment requirements as pt_set.
void pt_unmap(uint64_t va, size_t size);


// 1 GB-granule shortcut. Cheaper than going through L2 when the whole
// 1 GB region wants the same attributes. gb is the L1 index (0..511).
void pt_set_gb(int gb, pt_mem_type_t type);
void pt_unmap_gb(int gb);


// Make all pending changes visible: install promoted L2 tables, clean
// the descriptor caches, invalidate TLBs. Call once after a batch of
// pt_set / pt_unmap / pt_set_gb / pt_unmap_gb calls.
void pt_apply(void);
