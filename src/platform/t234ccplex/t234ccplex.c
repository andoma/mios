#include <stdint.h>
#include <stddef.h>
#include <malloc.h>
#include <string.h>

#include <mios/stream.h>
#include <mios/pmem.h>
#include <mios/error.h>
#include <mios/cli.h>
#include <mios/sys.h>

#include "t234_hsp.h"
#include "t234_bootinfo.h"
#include "t234_carveout_names.h"
#include "t234_reset_reason.h"

#include "t234ccplex_pmem.h"

#include "net/pbuf.h"

#include "cache.h"
#include "gicv3.h"
#include "pagetable.h"

pmem_t tegra_pmem;



long
gicr_base(void)
{
  long gicr0 = 0x0f440000;

  uint64_t v;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(v));
  int linear_core_id = ((v >> 16) & 0xff) * 4 + ((v >> 8) & 0xff);
  return gicr0 + linear_core_id * 0x20000;
}

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
      // Alias for BLANKET_NSDRAM — a marker covering the whole non-
      // secure DRAM range; the underlying memory is already classified
      // as Conventional/Cache-Coherent.
      continue;
    case CARVEOUT_UEFI:
    case CARVEOUT_RCM_BLOB:
      type = T234_PMEM_BOOT_SERVICES;
      break;
    case CARVEOUT_CCPLEX_INTERWORLD_SHMEM:
      // Holds the cpubl_params (cbp) we read at boot. CPU must keep
      // access; just don't allocate from it.
      type = T234_PMEM_UNUSABLE;
      break;
    default:
      // Engine firmware / Secure-side / firewalled. CPU access — even
      // speculative — fires a hardware abort; leave unmapped at EL1.
      type = T234_PMEM_INACCESSIBLE;
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


static void __attribute__((constructor(102)))
board_init_early(void)
{
  stdio = &tcu_early_console;

  extern void *load_addr;
  printf("\nMIOS on Tegra234 CCPLEX, Loaded at %p\n",
         load_addr);

  long id;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(id));
  printf("Core ID: 0x%lx\n", id);

  uint32_t reset_reason = (reg_rd(PMC_RESET_REASON_REGISTER) >> 2) & 0x3f;
  printf("Reset reason: %s\n",
         strtbl(t234_reset_reason, reset_reason));

  heap_add_mem(HEAP_START_EBSS, 0xffff000000200000ull + 2 * 1024 * 1024,
               0, 10);

  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64_unaligned(SCRATCH_BLINFO_LOCATION_REGISTER);

  const uint64_t sdram_end = cbp->sdram_base + cbp->sdram_size;
  printf("SDRAM from 0x%lx to 0x%lx\n", cbp->sdram_base, sdram_end);

  tegra_init_pmem(cbp);

  // Two-phase TTBR0 setup. Cortex-A78AE on Tegra234 speculatively
  // reaches into Device- and Normal-mapped PAs that the cluster
  // shouldn't be touching (low MMIO null pages, BPMP/SCE/etc.
  // firewalled carveouts). The speculative bus transaction hits a
  // no-slave or carveout-firewall response and EL3 takes the core
  // down via async RAS.
  //
  // Phase 1: install the L2/L3 unmaps that protect every firewalled
  // PA range. At this point all 1 GB blocks are still the Device
  // mapping set up by entry.S, so even if speculation sneaks into a
  // not-yet-protected PA it goes through Device (architecturally
  // un-speculatable) and any access to a now-invalid block faults at
  // the MMU before reaching the bus.
  pt_unmap(0, 2 * 1024 * 1024);
  for(size_t i = 0; i < tegra_pmem.count; i++) {
    const pmem_segment_t *s = &tegra_pmem.segments[i];
    if(s->type == T234_PMEM_INACCESSIBLE)
      pt_unmap(s->paddr, s->size);
  }
  pt_apply();

  // Phase 2: now that the unmaps are in TLBs, switch SDRAM to the
  // memory types we actually want. pt_set_gb preserves the invalids
  // we just installed so the firewalled regions stay protected even
  // after the rest of the GB becomes Normal-cached.
  //   First GB: Normal-NC (DMA descriptor rings live here; Normal-NC
  //             permits unaligned access — Device would alignment-
  //             fault on packed-struct stores.)
  //   Rest:     Normal-WB cacheable up to sdram_end.
  pt_set_gb(2, PT_NORMAL_NC);
  for(int gb = 3; gb < 512; gb++) {
    if(((uint64_t)gb << 30) >= sdram_end)
      pt_unmap_gb(gb);
    else
      pt_set_gb(gb, PT_NORMAL_WB);
  }
  pt_apply();

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


static void __attribute__((constructor(150)))
board_init_console(void)
{
  stdio = hsp_mbox_stream(NV_ADDRESS_MAP_TOP0_HSP_BASE, 0,
                          NV_ADDRESS_MAP_AON_HSP_BASE, 1);
}


const struct serial_number
sys_get_serial_number(void)
{
  static uint8_t sn[16] = {0,};
  struct serial_number serial  = {
    .data = sn,
    .len = 16
  };
  if (sn[0] != 0) {
    return serial;
  }

  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64_unaligned(SCRATCH_BLINFO_LOCATION_REGISTER);

  memcpy(sn, cbp->eeprom.cvm + 74, 15);
  sn[15] = 0;
  return serial;
}

static const char pmemtypestr[] =
  "???\0"
  "Conventional\0"
  "Cache-coherent\0"
  "Boot-services\0"
  "Runtime-services\0"
  "Reserved\0"
  "Loader\0"
  "Inaccessible\0"
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

CLI_CMD_DEF_EXT("show_pmem", cmd_pmem, NULL, "Show physical memory map");


static error_t
cmd_show_carveouts(cli_t *cli, int argc, char **argv)
{
  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64_unaligned(SCRATCH_BLINFO_LOCATION_REGISTER);

  cli_printf(cli, "cbp at 0x%016lx\n", (uint64_t)cbp);
  cli_printf(cli, "SDRAM 0x%016lx + 0x%lx\n",
             cbp->sdram_base, cbp->sdram_size);
  for(int i = 0; i < CARVEOUT_OEM_COUNT; i++) {
    const uint64_t base = (uint64_t)cbp->carveout_info[i].base;
    const uint64_t size = cbp->carveout_info[i].size;
    if(size == 0)
      continue;
    const int contains_cbp =
      ((uint64_t)cbp >= base && (uint64_t)cbp < base + size);
    cli_printf(cli, "  [%2d] %-26s 0x%010lx + 0x%010lx%s\n",
               i, strtbl(t234_carveout_names, i),
               base, size,
               contains_cbp ? "  <-- contains cbp" : "");
  }
  return 0;
}

CLI_CMD_DEF_EXT("show_carveouts", cmd_show_carveouts, NULL,
                "Show CPU bootloader params and carveout layout");



static error_t
cmd_cvm(cli_t *cli, int argc, char **argv)
{
  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64_unaligned(SCRATCH_BLINFO_LOCATION_REGISTER);

  hexdump("CVM", cbp->eeprom.cvm, 256);
  return 0;
}

CLI_CMD_DEF_EXT("show_cvm", cmd_cvm, NULL, "Show Tegra module EEPROM contents");
