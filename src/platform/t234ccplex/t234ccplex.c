#include <stdint.h>
#include <stddef.h>
#include <malloc.h>
#include <string.h>

#include <mios/stream.h>
#include <mios/pmem.h>
#include <mios/error.h>
#include <mios/cli.h>

#include "t234_hsp.h"
#include "t234_bootinfo.h"

#include "t234ccplex_pmem.h"

#include "net/pbuf.h"

#include "cache.h"

pmem_t tegra_pmem;

void
reboot(void)
{
  extern void smc(uint64_t cmd);
  smc(0x84000009);
}

void
outc(uint8_t c)
{
  uint32_t reg = NV_ADDRESS_MAP_AON_HSP_BASE + 0x10000 + 0x8000 * 1;
  while(reg_rd(reg)) {}
  reg_wr(reg, (1 << 31) | (1 << 24) | c);
}

static ssize_t
tcu_early_console_write(stream_t *s, const void *buf, size_t size, int flags)
{
  const uint8_t *n = buf;
  for(size_t i = 0; i < size; i++) {
    outc(n[i]);
  }
  return size;
}


static stream_vtable_t tcu_early_console_vtable = {
  .write = tcu_early_console_write,
};

static stream_t tcu_early_console = {
  .vtable = &tcu_early_console_vtable,
};



static void
tegra_init_pmem(const cpubl_params_v2_t *cbp)
{
  tegra_pmem.minimum_alignment = 4096;

  pmem_add(&tegra_pmem, cbp->sdram_base, cbp->sdram_size,
           T234_PMEM_CONVENTIONAL);

  // First GB is mapped DMA coherent by Mios
  const uint64_t GB = 1024 * 1024 * 1024;
  pmem_set(&tegra_pmem, cbp->sdram_base , GB, T234_PMEM_CACHE_COHERENT, 0);

  for(int i = 0; i < CARVEOUT_OEM_COUNT; i++) {
    if(cbp->carveout_info[i].size == 0)
      continue;

    int type;
    switch(i) {
    case CARVEOUT_VM_ENCRYPT:
      continue;
    case CARVEOUT_UEFI:
    case CARVEOUT_RCM_BLOB:
      type = T234_PMEM_BOOT_SERVICES;
      break;
    default:
      type = T234_PMEM_UNUSABLE;
      break;
    }
    pmem_set(&tegra_pmem, (uint64_t)cbp->carveout_info[i].base,
             cbp->carveout_info[i].size, type, 1);

  }
}



static void
add_heap_from_pmem(pmem_t *pmem, size_t size, uint32_t efi_type,
                   uint32_t mios_type, uint32_t prio, uint32_t orig_type)
{
  uint64_t paddr = pmem_alloc(pmem, size, orig_type, efi_type, 4096);
  heap_add_mem(paddr, paddr + size, mios_type, prio);
}

static void __attribute__((constructor(101)))
board_init_early(void)
{
  stdio = &tcu_early_console;

  extern void *load_addr;
  extern void *piggybacked_fdt;
  printf("\nMIOS on Tegra234 CCPLEX, Loaded at %p, FDT at %p\n",
         load_addr, piggybacked_fdt);

  heap_add_mem(HEAP_START_EBSS, 0xffff000000200000ull + 2 * 1024 * 1024,
               0, 10);

  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64(SCRATCH_BLINFO_LOCATION_REGISTER);

  const uint64_t sdram_end = cbp->sdram_base + cbp->sdram_size;
  printf("SDRAM from 0x%lx to 0x%lx\n", cbp->sdram_base, sdram_end);

  // Map SDRAM as follows
  // First GB - Non-cached
  // Reset of SDRAM - Cached
  // After end of SDRAM, make invalid

  uint64_t *ttbr0_el1;
  asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0_el1));

  for(int i = 3; i < 16; i++) {
    uint64_t paddr = (1ULL << 30) * i;
    if(paddr >= sdram_end) {
      ttbr0_el1[i] = 0;
    } else {
      ttbr0_el1[i] |= (1 << 2);
    }
  }

  cache_op(ttbr0_el1, 16*8, DCACHE_CLEAN_INV);
  asm volatile("dsb ishst;isb");
  asm volatile ("tlbi vmalle1; dsb ish; isb" ::: "memory");

  tegra_init_pmem(cbp);

  // Generic boot-time memory we can use
  add_heap_from_pmem(&tegra_pmem, 128 * 1048576,
                     T234_PMEM_BOOT_SERVICES,
                     MEM_TYPE_DMA, 20,
                     T234_PMEM_CONVENTIONAL);

  // Runtime services memory. This will be hands-off for Linux
  add_heap_from_pmem(&tegra_pmem, 65536,
                     T234_PMEM_RUNTIME_SERVICES,
                     MEM_TYPE_CHAINLOADER, 30,
                     T234_PMEM_CONVENTIONAL);

  // Generic boot-time cache-coherent memory we can use
  // Useful for DMA descriptor rings etc which are smaller than cache-line size
  add_heap_from_pmem(&tegra_pmem, 1048576,
                     T234_PMEM_BOOT_SERVICES,
                     MEM_TYPE_NO_CACHE, 40,
                     T234_PMEM_CACHE_COHERENT);

  // Allocate packet buffers from normal cached memory
  uint64_t pbuf_size = 1024*1024;
  uint64_t pbuf = pmem_alloc(&tegra_pmem, pbuf_size,
                             T234_PMEM_CONVENTIONAL,
                             T234_PMEM_BOOT_SERVICES, 4096);
  pbuf_data_add((void *)pbuf, (void *)(pbuf + pbuf_size));
}


static void __attribute__((constructor(110)))
board_init_console(void)
{
  stdio = hsp_mbox_stream(NV_ADDRESS_MAP_TOP0_HSP_BASE, 0,
                          NV_ADDRESS_MAP_AON_HSP_BASE, 1);
}



static const char pmemtypestr[] =
  "???\0"
  "Conventional\0"
  "Cache-coherent\0"
  "Boot-services\0"
  "Runtime-services\0"
  "Carveout/Unusable\0"
  "\0";

static error_t
cmd_pmem(cli_t *cli, int argc, char **argv)
{
  for(size_t i = 0; i < tegra_pmem.count; i++) {
    cli_printf(cli, "0x%016lx 0x%016lx %s\n",
               tegra_pmem.segments[i].paddr,
               tegra_pmem.segments[i].size,
               strtbl(pmemtypestr, tegra_pmem.segments[i].type));
  }
  return 0;
}

CLI_CMD_DEF("pmem", cmd_pmem);



static error_t
cmd_cvm(cli_t *cli, int argc, char **argv)
{
  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64(SCRATCH_BLINFO_LOCATION_REGISTER);

  hexdump("CVM", cbp->eeprom.cvm, 256);
  return 0;
}

CLI_CMD_DEF("cvm", cmd_cvm);
