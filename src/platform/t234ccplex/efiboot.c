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
#include <fdt/fdt.h>

#include "cache.h"

#include "util/crc32.h"

#include "t234_bootinfo.h"

#include "t234ccplex_pmem.h"
#include "t234ccplex_bpmp.h"

#include "efi.h"

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
                              T234_PMEM_CONVENTIONAL, memory_type, 4096);
  if(paddr == 0)
    return EFI_OUT_OF_RESOURCES;
  *out = paddr;
  return 0;
}

static efi_status_t
efi_boot_free_pages(efi_physical_addr_t paddr, unsigned long num_pages)
{
  pmem_set(&tegra_pmem, paddr, num_pages << 12, T234_PMEM_CONVENTIONAL);
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
  //  bs->exit_boot_services = efi_boot_exit_boot_services2;
}


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
  ctx.prop_cb = cleanup_fdt_prop;

  fdt_walk(&ctx);
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

  err = device_shutdown(NULL);
  if(err)
    return -1;


  return 0;
}



static error_t
startlinux2(const void *kbin, size_t ksize)
{
  const struct pe_header *pe = kbin;

  efi_image_handle_t *h = xalloc(sizeof(efi_image_handle_t),
                                 0, MEM_TYPE_CHAINLOADER |
                                 MEM_MAY_FAIL | MEM_CLEAR);
  if(h == NULL) {
    return ERR_NO_MEMORY;
  }

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
  h->system_table.nr_tables = 1;
  h->system_table.tables = h->config_tables;

  //  h->boot_services.exit_boot_services = efi_exit_boot_services;

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
  h->loaded_image.image_size = ksize;

  memcpy(h->loaded_image.image_base, kbin, ksize);

  cache_op(h->loaded_image.image_base, ksize, ICACHE_FLUSH);

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



static error_t __attribute__((unused))
startlinux(void)
{
  size_t ksize;
  void *kbin = kernel_from_rcmblob(&ksize);
  if(kbin == NULL) {
    return ERR_NOT_FOUND;
  }
  return startlinux2(kbin, ksize);
}


static error_t
cmd_linux(cli_t *cli, int argc, char **argv)
{
  return startlinux();
}

CLI_CMD_DEF("linux", cmd_linux);






#include "net/http/http_stream.h"


static void *loaded_kbin;
static size_t loaded_ksize;

static ssize_t
kload_write(struct stream *s, const void *buf, size_t size, int flags)
{
  memcpy(loaded_kbin + loaded_ksize, buf, size);
  loaded_ksize += size;
  return size;
}

static void
kload_close(stream_t *s)
{

}


static stream_vtable_t load_to_buf_vtable = {
  .write = kload_write,
  .close = kload_close,
};


static error_t
cmd_load(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  void *buf = xalloc(64 * 1024 * 1024, 0, MEM_MAY_FAIL);
  if(buf == NULL)
    return ERR_NO_MEMORY;
  loaded_kbin = buf;
  loaded_ksize = 0;
  cli_printf(cli, "Loading kernel to %p\n", buf);

  stream_t load_to_buf = {&load_to_buf_vtable};

  const char *url = argv[1];
  int64_t t0 = clock_get();
  error_t err = http_get(url, &load_to_buf, HTTP_FAIL_ON_ERROR, NULL, NULL, 0);
  int64_t t1 = clock_get();
  if(err) {
    free(loaded_kbin);
    return err;
  }
  cli_printf(cli, "Loaded %zd bytes in %d µs CRC:0x%08x\n", loaded_ksize,
             (int)(t1 - t0), crc32(0, loaded_kbin, loaded_ksize));

  err = startlinux2(loaded_kbin, loaded_ksize);

  free(loaded_kbin);
  loaded_kbin = NULL;
  return err;

}

CLI_CMD_DEF("load", cmd_load);
