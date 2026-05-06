#pragma once

#define T234_PMEM_CONVENTIONAL     1
#define T234_PMEM_CACHE_COHERENT   2
#define T234_PMEM_BOOT_SERVICES    3
#define T234_PMEM_RUNTIME_SERVICES 4
// Reserved: don't allocate from this, but the CPU may legitimately
// read/write it (e.g. CPU bootloader params shared with EL3).
#define T234_PMEM_UNUSABLE         5
#define T234_PMEM_LOADER           6
// Firewalled by hardware: CPU mustn't touch, even speculatively.
// EL1 stage-1 keeps these PA ranges unmapped.
#define T234_PMEM_INACCESSIBLE     7

