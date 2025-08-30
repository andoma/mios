#include <stdio.h>
#include <malloc.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include <mios/pmem.h>
#include <mios/cli.h>
#include <mios/task.h>
#include <mios/mios.h>
#include <mios/block.h>

#include <fdt/fdt.h>

#include "reg.h"
#include "util/crc32.h"
#include "t234_hsp.h"
#include "t234ccplex_qspi.h"

#include "efi.h"
#include <drivers/spiflash.h>

#include "cache.h"
#include "irq.h"

#define DEFAULT_BLINFO_LOCATION_ADDRESS  0x0C390154

typedef union {
  uint32_t version;
  uint8_t version_str[128];

} tegrabl_version_t;

typedef struct  {
  uint8_t cvm[256];
  uint8_t cvb[256];
  uint32_t cvm_size;
  uint32_t cvb_size;
} tegrabl_eeprom_data_t;

typedef struct {
  void *base;
  uint64_t size;
  uint64_t flags;
} tegrabl_carveout_info_t;

#define CARVEOUT_NONE                       0
#define CARVEOUT_NVDEC                      1
#define CARVEOUT_WPR1                       2
#define CARVEOUT_WPR2                       3
#define CARVEOUT_TSEC                       4
#define CARVEOUT_XUSB                       5
#define CARVEOUT_BPMP                       6
#define CARVEOUT_APE                        7
#define CARVEOUT_SPE                        8
#define CARVEOUT_SCE                        9
#define CARVEOUT_APR                        10
#define CARVEOUT_BPMP_DCE                   11
#define CARVEOUT_UNUSED3                    12
#define CARVEOUT_BPMP_RCE                   13
#define CARVEOUT_BPMP_MCE                   14
#define CARVEOUT_ETR                        15
#define CARVEOUT_BPMP_SPE                   16
#define CARVEOUT_RCE                        17
#define CARVEOUT_BPMP_CPUTZ                 18
#define CARVEOUT_PVA_FW                     19
#define CARVEOUT_DCE                        20
#define CARVEOUT_BPMP_PSC                   21
#define CARVEOUT_PSC                        22
#define CARVEOUT_NV_SC7                     23
#define CARVEOUT_CAMERA_TASKLIST            24
#define CARVEOUT_BPMP_SCE                   25
#define CARVEOUT_CV_GOS                     26
#define CARVEOUT_PSC_TSEC                   27
#define CARVEOUT_CCPLEX_INTERWORLD_SHMEM    28
#define CARVEOUT_FSI                        29
#define CARVEOUT_MCE                        30
#define CARVEOUT_CCPLEX_IST                 31
#define CARVEOUT_TSEC_HOST1X                32
#define CARVEOUT_PSC_TZ                     33
#define CARVEOUT_SCE_CPU_NS                 34
#define CARVEOUT_OEM_SC7                    35
#define CARVEOUT_SYNCPT_IGPU_RO             36
#define CARVEOUT_SYNCPT_IGPU_NA             37
#define CARVEOUT_VM_ENCRYPT                 38
#define CARVEOUT_BLANKET_NSDRAM             CARVEOUT_VM_ENCRYPT
#define CARVEOUT_CCPLEX_SMMU_PTW            39
#define CARVEOUT_DISP_EARLY_BOOT_FB         CARVEOUT_CCPLEX_SMMU_PTW
#define CARVEOUT_BPMP_CPU_NS                40
#define CARVEOUT_FSI_CPU_NS                 41
#define CARVEOUT_TSEC_DCE                   42
#define CARVEOUT_TZDRAM                     43
#define CARVEOUT_VPR                        44
#define CARVEOUT_MTS                        45
#define CARVEOUT_RCM_BLOB                   46
#define CARVEOUT_UEFI                       47
#define CARVEOUT_UEFI_MM_IPC                48
#define CARVEOUT_DRAM_ECC_TEST              49
#define CARVEOUT_PROFILING                  50
#define CARVEOUT_OS                         51
#define CARVEOUT_FSI_KEY_BLOB               52
#define CARVEOUT_TEMP_MB2RF                 53
#define CARVEOUT_TEMP_MB2_LOAD              54
#define CARVEOUT_TEMP_MB2_PARAMS            55
#define CARVEOUT_TEMP_MB2_IO_BUFFERS        56
#define CARVEOUT_TEMP_MB2RF_DATA            57
#define CARVEOUT_TEMP_MB2                   58
#define CARVEOUT_TEMP_MB2_SYSRAM_DATA       59
#define CARVEOUT_TSEC_CCPLEX                60
#define CARVEOUT_TEMP_MB2_APLT_LOAD         61
#define CARVEOUT_TEMP_MB2_APLT_PARAMS       62
#define CARVEOUT_TEMP_MB2_APLT_IO_BUFFERS   63
#define CARVEOUT_TEMP_MB2_APLT_SYSRAM_DATA  64
#define CARVEOUT_GR                         65
#define CARVEOUT_TEMP_QB_DATA               66
#define CARVEOUT_TEMP_QB_IO_BUFFER          67
#define CARVEOUT_ATF_FSI                    68
#define CARVEOUT_OPTEE_DTB                  69
#define CARVEOUT_UNUSED2                    70
#define CARVEOUT_UNUSED4                    71
#define CARVEOUT_RAM_OOPS                   72
#define CARVEOUT_OEM_COUNT                  73


typedef struct cpubl_params_v2 {
  uint8_t sha512_digest[64];

  uint32_t version;

  uint32_t uart_instance;

  uint32_t secure_os;

  uint32_t boot_type;

  uint32_t reserved1;

  uint32_t reserved2;

  uint64_t feature_flags;

  uint64_t sdram_base;

  uint64_t sdram_size;

  tegrabl_version_t mb1bct;

  tegrabl_version_t mb1;

  tegrabl_version_t mb2;

  tegrabl_eeprom_data_t eeprom __attribute__((aligned(8)));

  uint32_t boot_chain_selection_mode;

  uint32_t non_gpio_select_boot_chain;

  uint8_t brbct_custom_data[2048] __attribute__((aligned(8)));

  uint64_t dram_page_retirement_info_address;

  uint64_t reserved3; // Start address of hvinfo page

  uint64_t reserved4; // Start address of PVIT page

  uint32_t reserved5;
  uint32_t reserved6;

  uint8_t min_ratchet_level[304]  __attribute__((aligned(8)));

  tegrabl_carveout_info_t carveout_info[CARVEOUT_OEM_COUNT] __attribute__((aligned(8)));

} cpubl_params_v2_t;




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


pmem_t tegra_pmem;

static void
tegra_init_pmem(void)
{
  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64(DEFAULT_BLINFO_LOCATION_ADDRESS);

  tegra_pmem.minimum_alignment = 4096;

  pmem_add(&tegra_pmem, cbp->sdram_base, cbp->sdram_size,
           EFI_CONVENTIONAL_MEMORY);

  for(int i = 0; i < CARVEOUT_OEM_COUNT; i++) {

    if(cbp->carveout_info[i].size == 0)
      continue;

    int type;
    switch(i) {
    case CARVEOUT_VM_ENCRYPT:
      continue;
    case CARVEOUT_UEFI:
      type = EFI_BOOT_SERVICES_CODE;
      break;
    case CARVEOUT_RCM_BLOB:
      type = EFI_BOOT_SERVICES_DATA;
      break;
    default:
      type = EFI_UNUSABLE_MEMORY;
      break;
    }

    pmem_set(&tegra_pmem, (uint64_t)cbp->carveout_info[i].base,
             cbp->carveout_info[i].size, type);
  }
}

static error_t
cmd_pmem(cli_t *cli, int argc, char **argv)
{
  for(size_t i = 0; i < tegra_pmem.count; i++) {
    cli_printf(cli, "%016lx %016lx %d\n",
               tegra_pmem.segments[i].paddr,
               tegra_pmem.segments[i].size,
               tegra_pmem.segments[i].type);
  }
  return 0;
}

CLI_CMD_DEF("pmem", cmd_pmem);





static void
add_heap_from_pmem(pmem_t *pmem, size_t size, uint32_t efi_type, uint32_t mios_type,
                   uint32_t prio)
{
  uint64_t paddr = pmem_alloc(pmem, size, EFI_CONVENTIONAL_MEMORY, efi_type, 4096);
  heap_add_mem(paddr, paddr + size, mios_type, prio);

}

static void __attribute__((constructor(101)))
board_init_early(void)
{
  stdio = &tcu_early_console;

  extern void *load_addr;
  extern void *fdt_addr;
  printf("\nMIOS on Tegra234 CCPLEX, Loaded at %p, FDT at %p\n",
         load_addr, fdt_addr);

  heap_add_mem(HEAP_START_EBSS, 0xffff000000200000ull + 2 * 1024 * 1024,
               0, 10);

  // Map all SDRAM as normal cached
  uint64_t *ttbr0_el1;
  asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0_el1));

  for(int i = 2; i < 16; i++) {
    ttbr0_el1[i] |= (1 << 2);
    asm volatile("dc civac, %0" :: "r"(&ttbr0_el1[i]));
  }
  asm volatile("dsb ishst");
  asm volatile ("tlbi vmalle1; dsb ish; isb" ::: "memory");

  tegra_init_pmem();

  add_heap_from_pmem(&tegra_pmem, 65536, EFI_BOOT_SERVICES_DATA, MEM_TYPE_DMA, 20);
  add_heap_from_pmem(&tegra_pmem, 65536, EFI_RUNTIME_SERVICES_DATA,
                     MEM_TYPE_CHAINLOADER, 30);
}



static void
cbb_irq(void *arg)
{
  panic("CBB IRQ\n");
}


/*
[    0.072380] tegra234-cbb be00000.rce-fabric: secure IRQ: 158
[    0.072428] cbb tegra234_cbb_fault_enable ffff800008d99000
[    0.073396] tegra234-cbb c600000.aon-fabric: secure IRQ: 170
[    0.073426] cbb tegra234_cbb_fault_enable ffff800008fd7000
[    0.073508] tegra234-cbb d600000.bpmp-fabric: secure IRQ: 171
[    0.073530] cbb tegra234_cbb_fault_enable ffff800009059000
[    0.073596] tegra234-cbb de00000.dce-fabric: secure IRQ: 172
[    0.073618] cbb tegra234_cbb_fault_enable ffff8000090d9000
[    0.074500] tegra234-cbb 13a00000.cbb-fabric: secure IRQ: 176
[    0.074506] cbb tegra234_cbb_mask_serror ffff80000943a004
[    0.074529] cbb tegra234_cbb_fault_enable ffff800009460000
*/

static void __attribute__((constructor(1000)))
board_init_late(void)
{
  irq_enable_fn_arg(158, IRQ_LEVEL_CLOCK, cbb_irq, NULL);
  irq_enable_fn_arg(170, IRQ_LEVEL_CLOCK, cbb_irq, NULL);
  irq_enable_fn_arg(171, IRQ_LEVEL_CLOCK, cbb_irq, NULL);
  irq_enable_fn_arg(172, IRQ_LEVEL_CLOCK, cbb_irq, NULL);
  irq_enable_fn_arg(176, IRQ_LEVEL_CLOCK, cbb_irq, NULL);
}



static void __attribute__((constructor(110)))
board_init_console(void)
{
  stdio = hsp_mbox_stream(NV_ADDRESS_MAP_TOP0_HSP_BASE, 0,
                          NV_ADDRESS_MAP_AON_HSP_BASE, 1);
}


static error_t
cmd_carveout(cli_t *cli, int argc, char **argv)
{
  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64(DEFAULT_BLINFO_LOCATION_ADDRESS);

  //  sthexdump(stdio, "CBP", cbp, sizeof(cpubl_params_v2_t), 0);

  cli_printf(cli, "Bootloader info @ %p\n", cbp);
  cli_printf(cli, "\tversion:%d\n", cbp->version);
  cli_printf(cli, "\tsecure_os:%d\n", cbp->secure_os);
  cli_printf(cli, "\tboot_type:%d\n", cbp->boot_type);
  cli_printf(cli, "\treserved1:%d\n", cbp->reserved1);
  cli_printf(cli, "\treserved2:%d\n", cbp->reserved2);
  cli_printf(cli, "\tfeatureflags:0x%016lx\n", cbp->feature_flags);
  cli_printf(cli, "\tsdram_base:0x%016lx\n", cbp->sdram_base);
  cli_printf(cli, "\tsdram_size:0x%016lx\n", cbp->sdram_size);

  cli_printf(cli, "\tmb1bct: 0x%x %s\n", cbp->mb1bct.version, cbp->mb1bct.version_str);
  cli_printf(cli, "\tmb1:    0x%x %s\n", cbp->mb1.version, cbp->mb1.version_str);
  cli_printf(cli, "\tmb2:    0x%x %s\n", cbp->mb2.version, cbp->mb2.version_str);


  cli_printf(cli, "\teeprom sizes: 0x%x 0x%x\n",
         cbp->eeprom.cvm_size, cbp->eeprom.cvb_size);

  cli_printf(cli, "\tboot_chain_selection_mode: %d\n", cbp->boot_chain_selection_mode);
  cli_printf(cli, "\tnon_gpio_select_boot_chain: %d\n", cbp->non_gpio_select_boot_chain);
  cli_printf(cli, "\thvinfo:0x%016lx\n", cbp->reserved3);
  cli_printf(cli, "\tpvit:0x%016lx\n", cbp->reserved4);

  for(int i = 0; i < CARVEOUT_OEM_COUNT; i++) {
    cli_printf(cli, "\t %3d 0x%016lx 0x%016lx %lx\n",
               i,
               (uint64_t)cbp->carveout_info[i].base,
               cbp->carveout_info[i].size,
               cbp->carveout_info[i].flags);
  }
  return 0;
}

CLI_CMD_DEF("carveout", cmd_carveout);



struct rcmblob_header {
  uint8_t magic[4];
  uint8_t zero[4];
  uint8_t hash[64];
  uint32_t random_value;
  uint32_t num_items;

  struct {
    uint32_t type;
    uint32_t location;
    uint32_t zero;
    uint32_t length;
  } items[64];
};

static error_t
cmd_rcmblob(cli_t *cli, int argc, char **argv)
{
  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64(DEFAULT_BLINFO_LOCATION_ADDRESS);

  const struct rcmblob_header *rbh =
    (void *)cbp->carveout_info[CARVEOUT_RCM_BLOB].base;

  cli_printf(cli, "%d items\n", rbh->num_items);
  for(int i = 0; i < rbh->num_items; i++) {

    void *addr = (void *)rbh + rbh->items[i].location;
    cli_printf(cli, "%02x %08x %08x @ %p\n",
               rbh->items[i].type,
               rbh->items[i].location,
               rbh->items[i].length,
               addr);
  }
  return 0;

}


CLI_CMD_DEF("rcmblob", cmd_rcmblob);


static error_t
cmd_eeprom(cli_t *cli, int argc, char **argv)
{
  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64(DEFAULT_BLINFO_LOCATION_ADDRESS);

  sthexdump(cli->cl_stream, "CVM", cbp->eeprom.cvm, cbp->eeprom.cvm_size, 0);
  sthexdump(cli->cl_stream, "CVB", cbp->eeprom.cvb, cbp->eeprom.cvb_size, 0);
  return 0;
}

CLI_CMD_DEF("eeprom", cmd_eeprom);




static long efi_output_string(struct efi_simple_text_output_protocol *p,
                              uint16_t *char16)
{
  while(*char16) {
    char c = *char16;
    printf("%c", c);
    char16++;
  }
  return 0;
}


static efi_simple_text_output_protocol_t efi_console_output = {
  .output_string = efi_output_string,
};



static const uint8_t guid_device_tree[16] = {
  0xd5, 0x21, 0xb6, 0xb1, 0x9c, 0xf1, 0xa5, 0x41,
  0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0
};

static const uint8_t guid_loaded_image[16] = {
  0xa1, 0x31, 0x1b, 0x5b, 0x62, 0x95, 0xd2, 0x11,
  0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};


static efi_status_t
efi_boot_allocate_pages(int allocation_type, int memory_type,
                        unsigned long num_pages,
                        efi_physical_addr_t *out)
{
  if(allocation_type != 1)
    return EFI_INVALID_PARAMETER;

  uint64_t paddr = pmem_alloc(&tegra_pmem, num_pages << 12,
                              EFI_CONVENTIONAL_MEMORY, memory_type, 4096);
  if(paddr == 0)
    return EFI_OUT_OF_RESOURCES;
  *out = paddr;
  return 0;
}

static efi_status_t
efi_boot_free_pages(efi_physical_addr_t paddr, unsigned long num_pages)
{
  pmem_set(&tegra_pmem, paddr, num_pages << 12, EFI_CONVENTIONAL_MEMORY);
  return 0;
}

static efi_status_t
efi_boot_get_memory_map(unsigned long *map_size,
                        void *map,
                        unsigned long *key,
                        unsigned long *desc_size,
                        uint32_t *version)
{
  *desc_size = sizeof(efi_memory_desc_t);

  if(*map_size < sizeof(efi_memory_desc_t) * tegra_pmem.count) {
    *map_size = sizeof(efi_memory_desc_t) * tegra_pmem.count;
    return EFI_BUFFER_TOO_SMALL;
  }

  efi_memory_desc_t *descs = map;
  for(size_t i = 0; i < tegra_pmem.count; i++) {
    descs[i].type = tegra_pmem.segments[i].type;
    descs[i].phys_addr = tegra_pmem.segments[i].paddr;
    descs[i].virt_addr = 0;
    descs[i].num_pages = tegra_pmem.segments[i].size >> 12;
    descs[i].attribute = 0x100f;
  }

  *map_size = tegra_pmem.count * sizeof(efi_memory_desc_t);
  *version = 1;
  *key = crc32(0, tegra_pmem.segments, *map_size);
  return 0;
}


static efi_status_t
efi_boot_allocate_pool(int pool, unsigned long size, void **result)
{
  void *m = xalloc(size, 0, MEM_TYPE_CHAINLOADER);
  if(m == NULL)
    return EFI_OUT_OF_RESOURCES;
  *result = m;
  return 0;
}

static efi_status_t
efi_boot_free_pool(void *ptr)
{
  free(ptr);
  return 0;
}


static efi_status_t
efi_boot_create_event(uint32_t, unsigned long,
                      efi_event_notify_t, void *,
                      efi_event_t *)
{
  panic("%s", __FUNCTION__);
}


static efi_status_t
efi_boot_set_timer(efi_event_t,
                   EFI_TIMER_DELAY, uint64_t)
{
  panic("%s", __FUNCTION__);
}


static efi_status_t
efi_boot_wait_for_event(unsigned long,
                        efi_event_t *,
                        unsigned long *)
{
  panic("%s", __FUNCTION__);
}


static efi_status_t
efi_boot_close_event(efi_event_t)
{
  panic("%s", __FUNCTION__);
}


static efi_status_t
efi_boot_handle_protocol(efi_handle_t H,
                         efi_guid_t *guid, void **result)
{
  efi_image_handle_t *h = H;
  if(!memcmp(guid, guid_loaded_image, 16)) {
    *result = &h->loaded_image;
    return 0;
  }
  return EFI_UNSUPPORTED;
}

static efi_status_t
efi_boot_locate_handle(int, efi_guid_t *guid,
                       void *, unsigned long *,
                       efi_handle_t *)
{
  //  hexdump("efi_boot_locate_handle", guid, 16);
  return EFI_UNSUPPORTED;
}


static efi_status_t
efi_boot_locate_device_path(efi_guid_t *,
                            efi_device_path_protocol_t **,
                            efi_handle_t *)
{
  return EFI_UNSUPPORTED;
}

static efi_status_t
efi_boot_install_configuration_table(efi_guid_t *,
                                     void *)
{
  return EFI_UNSUPPORTED;

}


static efi_status_t
efi_boot_exit(efi_handle_t,
              efi_status_t,
              unsigned long,
              const uint16_t *)
{
  panic("%s", __FUNCTION__);
}

efi_status_t
efi_boot_exit_boot_services_prepare(efi_handle_t H,
                                    unsigned long mapkey)
{
  //  efi_image_handle_t *h = H;

  if(crc32(0, tegra_pmem.segments,
           tegra_pmem.count * sizeof(pmem_segment_t)) != mapkey) {
    return EFI_INVALID_PARAMETER;
  }
  return 0;
}


static efi_status_t
efi_boot_stall(unsigned long)
{
  panic("%s", __FUNCTION__);
}

static efi_status_t
efi_boot_disconnect_controller(efi_handle_t,
                               efi_handle_t,
                               efi_handle_t)
{
  panic("%s", __FUNCTION__);
}


static efi_status_t
efi_boot_locate_protocol(efi_guid_t *guid, void *, void **)
{
  //  hexdump("efi_boot_locate_protocol", guid, 16);
  return EFI_UNSUPPORTED;
}


static void
efi_init_boot_services(efi_boot_services_t *bs)
{
  bs->allocate_pages = efi_boot_allocate_pages;
  bs->free_pages = efi_boot_free_pages;
  bs->get_memory_map = efi_boot_get_memory_map;
  bs->allocate_pool = efi_boot_allocate_pool;
  bs->free_pool = efi_boot_free_pool;
  bs->create_event = efi_boot_create_event;
  bs->set_timer = efi_boot_set_timer;
  bs->wait_for_event = efi_boot_wait_for_event;
  bs->close_event = efi_boot_close_event;
  bs->handle_protocol = efi_boot_handle_protocol;
  bs->locate_handle = efi_boot_locate_handle;
  bs->locate_device_path = efi_boot_locate_device_path;
  bs->install_configuration_table = efi_boot_install_configuration_table;
  bs->exit = efi_boot_exit;
  bs->stall = efi_boot_stall;
  bs->disconnect_controller = efi_boot_disconnect_controller;
  bs->locate_protocol = efi_boot_locate_protocol;
}


static void *
kernel_from_rcmblob(size_t *sizep)
{
  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64(DEFAULT_BLINFO_LOCATION_ADDRESS);

  const struct rcmblob_header *rbh =
    (void *)cbp->carveout_info[CARVEOUT_RCM_BLOB].base;

  for(int i = 0; i < rbh->num_items; i++) {
    if(rbh->items[i].type == 0x2d) {
      *sizep = rbh->items[i].length;
      return (void *)rbh + rbh->items[i].location;
    }
  }
  return NULL;
}





static void *
efi_boot_thread(void *arg)
{
  efi_image_handle_t *h = arg;
  const struct pe_header *pe = h->loaded_image.image_base;

  int (*entrypoint)(void *handle, const efi_system_table_t *t);
  entrypoint = h->loaded_image.image_base + pe->entry_point;
  printf("Kernel entrypoint: %p  Let's go!\n", entrypoint);
  uint64_t r = entrypoint(h, &h->system_table);
  printf("Kernel failed to start, returncode: 0x%lx\n", r);
  return NULL;
}


static void
relocate_runtime_services(efi_image_handle_t *h)
{
  extern unsigned long _efi_runtime_begin;
  extern unsigned long _efi_runtime_end;

  void *paddr = (void *)pmem_alloc(&tegra_pmem, 4096,
                                   EFI_CONVENTIONAL_MEMORY,
                                   EFI_RUNTIME_SERVICES_CODE,
                                   4096);

  void *efi_runtime_begin = (void *)&_efi_runtime_begin;
  void *efi_runtime_end = (void *)&_efi_runtime_end;

  const size_t efi_runtime_size = efi_runtime_end - efi_runtime_begin;

  memcpy(paddr, efi_runtime_begin, efi_runtime_size);

  size_t init_offset = (void *)&efi_init_runtime_services - efi_runtime_begin;
  dcache_op(paddr, efi_runtime_size, DCACHE_CLEAN);
  icache_invalidate();

  void (*init)(efi_image_handle_t *h) = paddr + init_offset;
  init(h);
}


static uint32_t cpu_phandle[12];


static int
cleanup_fdt_node(void *opaque, fdt_walkctx_t *ctx)
{
  if(fdt_walk_match_node_name("/cpus/cpu@10000", ctx) ||
     fdt_walk_match_node_name("/cpus/cpu@10100", ctx) ||
     fdt_walk_match_node_name("/cpus/cpu@20000", ctx) ||
     fdt_walk_match_node_name("/cpus/cpu@20100", ctx) ||
     fdt_walk_match_node_name("/cpus/cpu@20200", ctx) ||
     fdt_walk_match_node_name("/cpus/cpu@20300", ctx))
    return 1;
  return 0;
}

static size_t
replace_cpu_cooling_map(void *data, size_t len)
{
  uint32_t *u32 = data;
  u32[0] = cpu_phandle[0];
  u32[3] = cpu_phandle[2];
  u32[6] = cpu_phandle[6];
  return 9 * 4;
}

static size_t
cleanup_fdt_prop(void *opaque, struct fdt_walkctx *ctx, const char *name,
                 void *data, size_t len)
{
  if(!strcmp(name, "phandle")) {
    if(fdt_walk_match_node_name("/cpus/cpu@0", ctx))
      memcpy(&cpu_phandle[0], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@100", ctx))
      memcpy(&cpu_phandle[1], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@200", ctx))
      memcpy(&cpu_phandle[2], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@300", ctx))
      memcpy(&cpu_phandle[3], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@10000", ctx))
      memcpy(&cpu_phandle[4], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@10100", ctx))
      memcpy(&cpu_phandle[5], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@10200", ctx))
      memcpy(&cpu_phandle[6], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@10300", ctx))
      memcpy(&cpu_phandle[7], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@20000", ctx))
      memcpy(&cpu_phandle[8], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@20100", ctx))
      memcpy(&cpu_phandle[9], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@20200", ctx))
      memcpy(&cpu_phandle[10], data, 4);
    if(fdt_walk_match_node_name("/cpus/cpu@20300", ctx))
      memcpy(&cpu_phandle[11], data, 4);
  }

  if(!strcmp(name, "cooling-device")) {
    if(fdt_walk_match_node_name("/thermal-zones/cpu-thermal/cooling-maps/map-cpufreq", ctx)) {
      return replace_cpu_cooling_map(data, len);
    }
    if(fdt_walk_match_node_name("/thermal-zones/gpu-thermal/cooling-maps/map-cpufreq", ctx)) {
      return replace_cpu_cooling_map(data, len);
    }
    if(fdt_walk_match_node_name("/thermal-zones/cv0-thermal/cooling-maps/map-cpufreq", ctx)) {
      return replace_cpu_cooling_map(data, len);
    }
    if(fdt_walk_match_node_name("/thermal-zones/cv1-thermal/cooling-maps/map-cpufreq", ctx)) {
      return replace_cpu_cooling_map(data, len);
    }
    if(fdt_walk_match_node_name("/thermal-zones/cv2-thermal/cooling-maps/map-cpufreq", ctx)) {
      return replace_cpu_cooling_map(data, len);
    }
    if(fdt_walk_match_node_name("/thermal-zones/soc0-thermal/cooling-maps/map-cpufreq", ctx)) {
      return replace_cpu_cooling_map(data, len);
    }
    if(fdt_walk_match_node_name("/thermal-zones/soc1-thermal/cooling-maps/map-cpufreq", ctx)) {
      return replace_cpu_cooling_map(data, len);
    }
    if(fdt_walk_match_node_name("/thermal-zones/soc2-thermal/cooling-maps/map-cpufreq", ctx)) {
      return replace_cpu_cooling_map(data, len);
    }

  }
  return len;
}


static void
fdt_cleanup(void *fdt_addr)
{
  fdt_walkctx_t ctx;
  fdt_init_walkctx(&ctx, fdt_addr);

  ctx.node_cb = cleanup_fdt_node;
  if(0)
    ctx.prop_cb = cleanup_fdt_prop;

  fdt_walk(&ctx);
}



static error_t __attribute__((unused))
startlinux(void)
{
  size_t ksize;
  void *kbin = kernel_from_rcmblob(&ksize);
  if(kbin == NULL) {
    return ERR_NOT_FOUND;
  }

  const struct pe_header *pe = kbin;

  efi_image_handle_t *h = xalloc(sizeof(efi_image_handle_t),
                                 0, MEM_TYPE_CHAINLOADER |
                                 MEM_MAY_FAIL | MEM_CLEAR);
  if(h == NULL) {
    return ERR_NO_MEMORY;
  }

  h->system_table.hdr.signature = 0x5453595320494249ULL;
  h->system_table.hdr.revision = 0x20000; // v2.0

  h->fw_vendor[0] = 'm';
  h->fw_vendor[1] = 'i';
  h->fw_vendor[2] = 'o';
  h->fw_vendor[3] = 's';
  h->fw_vendor[4] = 0;

  h->system_table.fw_vendor = h->fw_vendor;
  h->system_table.con_out = &efi_console_output;
  h->system_table.boot = &h->boot_services;
  h->system_table.runtime = &h->runtime_services;
  h->system_table.nr_tables = 1;
  h->system_table.tables = h->config_tables;

  efi_init_boot_services(&h->boot_services);
  relocate_runtime_services(h);

  size_t kalign = 65536; // TODO: Read from PE header
  size_t memsiz = (pe->code_size + pe->initialized_data_size + kalign) & ~kalign;
  uint64_t paddr = pmem_alloc(&tegra_pmem, memsiz + 16 * 1024 * 1024,
                              EFI_CONVENTIONAL_MEMORY, EFI_LOADER_CODE,
                              kalign);
  if(paddr == 0) {
    free(h);
    return ERR_NO_MEMORY;
  }

  h->loaded_image.image_base = (void *)paddr;
  h->loaded_image.image_size = ksize;

  memcpy(h->loaded_image.image_base, kbin, ksize);

  dcache_op(h->loaded_image.image_base, ksize, DCACHE_CLEAN);
  icache_invalidate();

  memcpy(&h->config_tables[0].guid, guid_device_tree, 16);

  extern void *fdt_addr;

  fdt_cleanup(fdt_addr);

  h->config_tables[0].table = fdt_addr;

  thread_t *t = thread_create(efi_boot_thread, h, 0, "efiboot",
                              TASK_NO_FPU |
                              (MEM_TYPE_CHAINLOADER << TASK_MEMTYPE_SHIFT),
                              5);
  thread_join(t);

  // If we come back the kernel failed to launch, so clean up
  pmem_set(&tegra_pmem,
           (uint64_t)h->loaded_image.image_base,
           memsiz, EFI_CONVENTIONAL_MEMORY);
  free(h);
  return 0;
}

static error_t
cmd_linux(cli_t *cli, int argc, char **argv)
{
  return startlinux();
}

CLI_CMD_DEF("linux", cmd_linux);

int
main(void)
{
#if 0
  spi_t *qspi = tegra_qspi_init();
  block_iface_t *bi = spiflash_create(qspi, GPIO_UNUSED);
  printf("bi=%p\n", bi);
#endif
  cli_console('#');
}
