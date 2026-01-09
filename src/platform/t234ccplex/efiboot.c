#include <mios/efi.h>

#include <stdint.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mios/pmem.h>
#include <mios/mios.h>
#include <mios/device.h>
#include <mios/task.h>
#include <mios/cli.h>

#include "cache.h"

#include "util/crc32.h"

#include "t234_bootinfo.h"

#include "t234ccplex_pmem.h"
#include "t234ccplex_bpmp.h"

#include "efi_internal.h"
#include "smbios.h"
extern void *FDT;

/**
 * Some day it might be better to separate this from tegra specifics
 * But it's too early of an abstraction right now
 */

extern pmem_t tegra_pmem;

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


static void
set_cmdline(efi_loaded_image_t *li, const char *str)
{
  if(str == NULL)
    return;

  size_t len = strlen(str) + 1;
  uint16_t *u16 = malloc(len * 2);
  for(size_t i = 0; i < len; i++) {
    u16[i] = str[i];
  }
  li->load_options_size = len * 2;
  li->load_options = u16;
}


static efi_simple_text_output_protocol_t efi_console_output = {
  .output_string = efi_output_string,
};


static const uint8_t guid_smbios3_table[16] = {
  0x44, 0x15, 0xfd, 0xf2, 0x94, 0x97, 0x2c, 0x4a,
  0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x20, 0xE3, 0x94
};

static const uint8_t guid_rt_properties_table[16] = {
  0x8a, 0x91, 0x66, 0xeb, 0xef, 0x7e, 0x2a, 0x40,
  0x84, 0x2e, 0x93, 0x1d, 0x21, 0xc3, 0x8a, 0xe9
};

static const uint8_t guid_device_tree[16] = {
  0xd5, 0x21, 0xb6, 0xb1, 0x9c, 0xf1, 0xa5, 0x41,
  0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0
};

static const uint8_t guid_loaded_image[16] = {
  0xa1, 0x31, 0x1b, 0x5b, 0x62, 0x95, 0xd2, 0x11,
  0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b
};

static const uint8_t guid_rng[16] = {
  0xa5, 0xbc, 0x52, 0x31, 0xde, 0xea, 0x3d, 0x43,
  0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44
};


static efi_status_t
efi_boot_allocate_pages(int allocation_type, int memory_type,
                        unsigned long num_pages,
                        efi_physical_addr_t *out)
{
  int t234_mem = 0;

  switch(memory_type) {
  case EFI_LOADER_CODE:
  case EFI_LOADER_DATA:
    t234_mem = T234_PMEM_LOADER;
    break;

  case EFI_BOOT_SERVICES_CODE:
  case EFI_BOOT_SERVICES_DATA:
    t234_mem = T234_PMEM_BOOT_SERVICES;
    break;
  case EFI_RUNTIME_SERVICES_CODE:
  case EFI_RUNTIME_SERVICES_DATA:
    t234_mem = T234_PMEM_RUNTIME_SERVICES;
    break;

  case EFI_CONVENTIONAL_MEMORY:
    t234_mem = T234_PMEM_CONVENTIONAL;
    break;

  case EFI_UNUSABLE_MEMORY:
  case EFI_ACPI_RECLAIM_MEMORY:
  case EFI_ACPI_MEMORY_NVS:
  case EFI_MEMORY_MAPPED_IO:
  case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
  case EFI_PAL_CODE:
  case EFI_PERSISTENT_MEMORY:
    return EFI_INVALID_PARAMETER;
  }

  uint64_t paddr;
  int64_t size = num_pages << 12;

  switch(allocation_type) {
  case 1: // AllocateMaxAddress
    paddr = pmem_alloc(&tegra_pmem, size,
                       T234_PMEM_CONVENTIONAL, t234_mem, 4096);
    if(paddr == 0)
      return EFI_OUT_OF_RESOURCES;
    *out = paddr;
    return 0;

  case 2: // AllocateAddress
    paddr = *out;
    if(pmem_set(&tegra_pmem, *out, size, t234_mem, 0))
      return EFI_NOT_FOUND;
    return 0;
  default:
    return EFI_INVALID_PARAMETER;
  }
}

static efi_status_t
efi_boot_free_pages(efi_physical_addr_t paddr, unsigned long num_pages)
{
  pmem_set(&tegra_pmem, paddr, num_pages << 12, T234_PMEM_CONVENTIONAL, 0);
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

    int efi_type = 0;
    uint64_t attrib = EFI_MEMORY_WB;


    switch(tegra_pmem.segments[i].type) {
    case T234_PMEM_CONVENTIONAL:
    case T234_PMEM_CACHE_COHERENT:
      efi_type = EFI_CONVENTIONAL_MEMORY;
      break;
    case T234_PMEM_BOOT_SERVICES:
      efi_type = EFI_BOOT_SERVICES_DATA;
      break;
    case T234_PMEM_RUNTIME_SERVICES:
      efi_type = EFI_RUNTIME_SERVICES_CODE;
      attrib |= EFI_MEMORY_RUNTIME;
      break;
    case T234_PMEM_UNUSABLE:
      efi_type = EFI_UNUSABLE_MEMORY;
      break;
    case T234_PMEM_LOADER:
      efi_type = EFI_LOADER_CODE;
      break;
    default:
      panic("Can't map pmem type %d to efi type", tegra_pmem.segments[i].type);
    }

    descs[i].type = efi_type;
    descs[i].phys_addr = tegra_pmem.segments[i].paddr;
    descs[i].virt_addr = 0;
    descs[i].num_pages = tegra_pmem.segments[i].size >> 12;
    descs[i].attribute = attrib;
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
  return 0;
}


static efi_status_t
efi_boot_exit(efi_handle_t,
              efi_status_t,
              unsigned long,
              const uint16_t *)
{
  panic("%s", __FUNCTION__);
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
efi_rng_get_info(void *self, unsigned long *a, efi_guid_t *guid)
{
  return EFI_UNSUPPORTED;
}


static efi_status_t
efi_rng_get_rng(void *self,
                efi_guid_t *guid, unsigned long size,
                uint8_t *ptr)
{
  for(size_t i = 0; i < size; i++)
    ptr[i] = rand();
  return 0;
}


static struct {
  efi_status_t (*get_info)(void *self, unsigned long *a, efi_guid_t *guid);

  efi_status_t (*get_rng)(void *self,
                          efi_guid_t *guid, unsigned long size,
                          uint8_t *ptr);
} efi_rng = {
  .get_info = efi_rng_get_info,
  .get_rng = efi_rng_get_rng
};


static efi_status_t
efi_boot_locate_protocol(efi_guid_t *guid, void *, void **result)
{
  if(!memcmp(guid, guid_rng, 16)) {
    *result = &efi_rng;
    return 0;
  }

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
  //  bs->exit_boot_services = efi_boot_exit_boot_services2;
}


#if 0
static void *
kernel_from_rcmblob(size_t *sizep)
{
  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64(SCRATCH_BLINFO_LOCATION_REGISTER);

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
#endif




static void *
efi_boot_thread(void *arg)
{
  efi_image_handle_t *h = arg;
  const struct pe_header *pe = h->loaded_image.image_base;

  int (*entrypoint)(void *handle, const efi_system_table_t *t);
  entrypoint = h->loaded_image.image_base + pe->entry_point;
  printf("Kernel initializing...\n");
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
                                   T234_PMEM_CONVENTIONAL,
                                   T234_PMEM_RUNTIME_SERVICES,
                                   4096);

  void *efi_runtime_begin = (void *)&_efi_runtime_begin;
  void *efi_runtime_end = (void *)&_efi_runtime_end;

  const size_t efi_runtime_size = efi_runtime_end - efi_runtime_begin;

  memcpy(paddr, efi_runtime_begin, efi_runtime_size);

  size_t init_offset = (void *)&efi_init_runtime_services - efi_runtime_begin;
  cache_op(paddr, efi_runtime_size, ICACHE_FLUSH);

  void (*init)(efi_image_handle_t *h) = paddr + init_offset;
  init(h);
}


static int
prep_exit_boot_services(void)
{
  error_t err;

  if((err = bpmp_powergate_set(10, 1)) != 0)
    return -1;
  if((err = bpmp_powergate_set(12, 1)) != 0)
    return -1;
  if((err = bpmp_rst_set(114, 1)) != 0)
    return -1;

  shutdown_notification("EFI Loader");

  printf("Shutting down devices ... ");
  err = device_shutdown(NULL);
  if(err)
    return -1;
  printf("done.\nBootloader transfers control to kernel.\n");

  return 0;
}

error_t
efi_exec(const void *bin, size_t size, const char *cmdline)
{
  const struct pe_header *pe = bin;

  // TODO: Check that we have a reasonable pe_header

  efi_image_handle_t *h = xalloc(sizeof(efi_image_handle_t),
                                 0, MEM_TYPE_CHAINLOADER |
                                 MEM_MAY_FAIL | MEM_CLEAR);
  if(h == NULL) {
    return ERR_NO_MEMORY;
  }

  build_and_install_smbios3(h->smbios, sizeof(h->smbios));

  h->rt_proptable.version = 1;
  h->rt_proptable.length = 8;
  h->rt_proptable.runtime_services_supported = 0x400; // We only support reset

  h->prep_exit_boot_services = prep_exit_boot_services;
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
  h->system_table.nr_tables = 3;
  h->system_table.tables = h->config_tables;

  efi_init_boot_services(&h->boot_services);
  relocate_runtime_services(h);

  size_t kalign = 65536; // TODO: Read from PE header
  size_t memsiz = (pe->code_size + pe->initialized_data_size + kalign) & ~kalign;
  uint64_t paddr = pmem_alloc(&tegra_pmem, memsiz + 16 * 1024 * 1024,
                              T234_PMEM_CONVENTIONAL, T234_PMEM_LOADER,
                              kalign);
  if(paddr == 0) {
    free(h);
    return ERR_NO_MEMORY;
  }

  h->loaded_image.image_base = (void *)paddr;
  h->loaded_image.image_size = size;
  printf("cmdline: %s\n", cmdline);
  set_cmdline(&h->loaded_image, cmdline);

  memcpy(h->loaded_image.image_base, bin, size);

  cache_op(h->loaded_image.image_base, size, ICACHE_FLUSH);

  memcpy(&h->config_tables[0].guid, guid_device_tree, 16);
  h->config_tables[0].table = FDT;

  memcpy(&h->config_tables[1].guid, guid_rt_properties_table, 16);
  h->config_tables[1].table = &h->rt_proptable;

  memcpy(&h->config_tables[2].guid, guid_smbios3_table, 16);
  h->config_tables[2].table = h->smbios;

  thread_t *t = thread_create(efi_boot_thread, h, 0, "efiboot",
                              TASK_NO_FPU |
                              (MEM_TYPE_CHAINLOADER << TASK_MEMTYPE_SHIFT),
                              5);
  thread_join(t);

  // If we come back the kernel failed to launch, so clean up
  pmem_set(&tegra_pmem,
           (uint64_t)h->loaded_image.image_base,
           memsiz, EFI_CONVENTIONAL_MEMORY, 0);

  free(h->loaded_image.load_options);
  free(h);
  return 0;
}
