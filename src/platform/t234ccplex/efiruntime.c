#include "efi.h"
#include <stddef.h>

#include <stdio.h>

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_get_time(void *tm, void *tc)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_set_time(void *tm)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_get_wakeup_time(bool *enabled, bool *pending, void *tm)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_set_wakeup_time(bool enabled, void *tm)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_set_virtual_address_map(unsigned long memory_map_size,
                            unsigned long descriptor_size,
                            uint32_t descriptor_version,
                            efi_memory_desc_t *virtual_map)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_get_variable(uint16_t *name, efi_guid_t *vendor, uint32_t *attr,
                 unsigned long *data_size, void *data)
{
  return EFI_NOT_FOUND;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_get_next_variable(unsigned long *name_size, uint16_t *name,
                      efi_guid_t *vendor)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_set_variable(uint16_t *name, efi_guid_t *vendor,
                 uint32_t attr, unsigned long data_size,
                 void *data)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_get_next_high_mono_count(uint32_t *count)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_reset_system(int reset_type, efi_status_t status,
                 unsigned long data_size, uint16_t *data)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_update_capsule(efi_capsule_header_t **capsules,
                   unsigned long count,
                   unsigned long sg_list)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_query_capsule_caps(efi_capsule_header_t **capsules,
                        unsigned long count,
                        uint64_t *max_size,
                        int *reset_type)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_query_variable_store(uint32_t attributes,
                         unsigned long size,
                         bool nonblocking)
{
  return EFI_UNSUPPORTED;
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_exit_boot_services(efi_handle_t H,
                       unsigned long mapkey)
{
  asm volatile ("hvc #0" ::: "memory");
  return 0;
}

__attribute__((section("efi_runtime"),noinline))
void
efi_init_runtime_services(efi_image_handle_t *h)
{
  efi_runtime_services_t *rs = &h->runtime_services;
  rs->get_time = efi_get_time;
  rs->set_time = efi_set_time;
  rs->get_wakeup_time = efi_get_wakeup_time;
  rs->set_wakeup_time = efi_set_wakeup_time;
  rs->set_virtual_address_map = efi_set_virtual_address_map;
  rs->get_variable = efi_get_variable;
  rs->get_next_variable = efi_get_next_variable;
  rs->get_next_high_mono_count = efi_get_next_high_mono_count;
  rs->set_variable = efi_set_variable;
  rs->reset_system = efi_reset_system;
  rs->update_capsule = efi_update_capsule;
  rs->query_capsule_caps = efi_query_capsule_caps;
  rs->query_variable_store = efi_query_variable_store;

  h->boot_services.exit_boot_services = efi_exit_boot_services;
}
