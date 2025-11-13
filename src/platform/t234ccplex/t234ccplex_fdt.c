#include <fdt/fdt.h>

#include <mios/eventlog.h>
#include <mios/cli.h>
#include <mios/sys.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <malloc.h>

#include "t234ccplex_ari.h"

#include "t234_bootinfo.h"

void *FDT;


/**
 * Linux expects the FDT to only contain the available CPUs so we have
 * to clean that up
 *
 * We also add a top level node "serial-number" with the serial number
 * from the jetson module eeprom (CVM)
 *
 */

#define MAX_CPUS 12

// Check if s1 begins with s2
static const char *
begins(const char *s1, const char *s2)
{
  while(*s2)
    if(*s1++ != *s2++)
      return NULL;
  return s1;
}


static void __attribute__((constructor(1000)))
fdt_init(void)
{
  extern const char builtin_fdt[];

  uint32_t cpu_phandle[MAX_CPUS] = {0};

  fdt_t fdt = {(void *)builtin_fdt};
  fdt.capacity = fdt_get_totalsize(&fdt);

  const char *err = fdt_validate(&fdt);
  if(err) {
    evlog(LOG_ERR, "Incoming FDT is malformed: %s", err);
    return;
  }

  uint64_t coremask;


  if(ari_cmd(TEGRA_ARI_NUM_CORES_CMD, 0, &coremask)) {
    evlog(LOG_ERR, "Unable to get available CPUs");
    free(fdt.buffer);
    return;
  }

  // Add some extra space for modifications
  size_t copy_size = fdt.capacity + 1024;
  void *copy = xalloc(copy_size, 0, MEM_MAY_FAIL);
  if(copy == NULL) {
    evlog(LOG_ERR, "No memory for modified FDT");
    return;
  }

  memcpy(copy, builtin_fdt, fdt.capacity);
  fdt.buffer = copy;
  fdt.capacity = copy_size;

  fdt_node_ref_t key;

  // Remove CPU cores which are not present in system
  // Also collect phandles for each core for use further down

  const char *match;
  key = 0;
  while((key = fdt_find_next_node(&fdt, key, "/cpus/*", &match)) != 0) {
    const char *cpuid = begins(match, "cpu@");
    if(cpuid == NULL)
      continue;

    const int phys_id = xtoi(match + 4);
    const int id = ((phys_id >> 8) & 0x3) | ((phys_id >> 14) & 0xc);

    if(!((1 << id) & coremask)) {
      fdt_erase_node(&fdt, key);
      continue;
    }

    if(id >= MAX_CPUS)
      continue;
    size_t phlen;
    const void *ph = fdt_get_property(&fdt, key, "phandle", &phlen);
    if(phlen == sizeof(uint32_t)) {
      memcpy(&cpu_phandle[id], ph, sizeof(uint32_t));
    }
  }

  // Construct new cooling_device array for thermal-zones
  // Each pair of core is considered one
  size_t num_cooling_device = 0;
  for(size_t i = 0; i < MAX_CPUS; i += 2) {
    if(cpu_phandle[i])
      num_cooling_device++;
  }

  size_t cooling_device_size = num_cooling_device * 3 * sizeof(uint32_t);
  uint32_t *cooling_device = xalloc(cooling_device_size, 0, MEM_MAY_FAIL);

  if(!cooling_device) {
    evlog(LOG_ERR, "No memory for cooling device array");
    free(fdt.buffer);
    return;
  }

  size_t j = 0;
  for(size_t i = 0; i < MAX_CPUS; i += 2) {
    if(cpu_phandle[i]) {
      cooling_device[j++] = cpu_phandle[i];
      cooling_device[j++] = ~0;
      cooling_device[j++] = ~0;
    }
  }

  key = 0;
  while((key = fdt_find_next_node(&fdt, key,
                                  "/thermal-zones/*/cooling-maps/map-cpufreq",
                                  &match)) != 0) {
    fdt_set_property(&fdt, key, "cooling-device", cooling_device,
                     cooling_device_size);
  }
  free(cooling_device);
  const struct serial_number sn = sys_get_serial_number();
  // Update serial-number in FDT
  key = fdt_find_next_node(&fdt, 0, "", NULL);
  if(key) {
    fdt_set_property(&fdt, key, "serial-number", sn.data, sn.len);
  }

  FDT = fdt.buffer;
}

