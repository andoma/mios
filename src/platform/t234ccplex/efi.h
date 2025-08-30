#pragma once

#include <stdint.h>
#include <stdbool.h>

#define EFI_RESERVED_TYPE		 0
#define EFI_LOADER_CODE			 1
#define EFI_LOADER_DATA			 2
#define EFI_BOOT_SERVICES_CODE		 3
#define EFI_BOOT_SERVICES_DATA		 4
#define EFI_RUNTIME_SERVICES_CODE	 5
#define EFI_RUNTIME_SERVICES_DATA	 6
#define EFI_CONVENTIONAL_MEMORY		 7
#define EFI_UNUSABLE_MEMORY		 8
#define EFI_ACPI_RECLAIM_MEMORY		 9
#define EFI_ACPI_MEMORY_NVS		10
#define EFI_MEMORY_MAPPED_IO		11
#define EFI_MEMORY_MAPPED_IO_PORT_SPACE	12
#define EFI_PAL_CODE			13
#define EFI_PERSISTENT_MEMORY		14


#define EFI_SUCCESS		0
#define EFI_LOAD_ERROR		( 1 | (1UL << 63))
#define EFI_INVALID_PARAMETER	( 2 | (1UL << 63))
#define EFI_UNSUPPORTED		( 3 | (1UL << 63))
#define EFI_BAD_BUFFER_SIZE	( 4 | (1UL << 63))
#define EFI_BUFFER_TOO_SMALL	( 5 | (1UL << 63))
#define EFI_NOT_READY		( 6 | (1UL << 63))
#define EFI_DEVICE_ERROR	( 7 | (1UL << 63))
#define EFI_WRITE_PROTECTED	( 8 | (1UL << 63))
#define EFI_OUT_OF_RESOURCES	( 9 | (1UL << 63))
#define EFI_NOT_FOUND		(14 | (1UL << 63))
#define EFI_TIMEOUT		(18 | (1UL << 63))
#define EFI_ABORTED		(21 | (1UL << 63))
#define EFI_SECURITY_VIOLATION	(26 | (1UL << 63))




struct pe_header {
  uint8_t hdr[64];
  uint32_t PE;
  uint16_t arm64;
  uint16_t section_count;
  uint32_t timestamp;
  uint32_t symboltable;
  uint32_t number_of_symbols;
  uint16_t size_of_optional_header;
  uint16_t characteristics;
  uint16_t opt_header_magic;
  uint8_t major_linker_version;
  uint8_t minor_linker_version;
  uint32_t code_size;
  uint32_t initialized_data_size;
  uint32_t uninitialized_data_size;
  uint32_t entry_point;
};


typedef	struct {
  uint64_t signature;
  uint32_t revision;
  uint32_t headersize;
  uint32_t crc32;
  uint32_t reserved;
} efi_table_hdr_t;


typedef void *efi_handle_t;
typedef unsigned long efi_status_t;
typedef uint64_t efi_physical_addr_t;

typedef struct { uint32_t a; uint16_t b; uint16_t c; uint8_t d[8]; } efi_guid_t;

typedef struct {
  uint32_t type;
  uint32_t pad;
  uint64_t phys_addr;
  uint64_t virt_addr;
  uint64_t num_pages;
  uint64_t attribute;
} efi_memory_desc_t;



typedef struct {
  efi_guid_t guid;
  uint32_t headersize;
  uint32_t flags;
  uint32_t imagesize;
} efi_capsule_header_t;




typedef void *efi_event_t;
typedef void (*efi_event_notify_t)(efi_event_t, void *);
typedef struct efi_generic_dev_path efi_device_path_protocol_t;

typedef enum {
	EfiTimerCancel,
	EfiTimerPeriodic,
	EfiTimerRelative
} EFI_TIMER_DELAY;

typedef struct {
  uint8_t guid[16];
  void *table;
} efi_config_table_t;



typedef struct {
  efi_table_hdr_t hdr;
  efi_status_t (*get_time)(void *tm, void *tc);
  efi_status_t (*set_time)(void *tm);
  efi_status_t (*get_wakeup_time)(bool *enabled, bool *pending,
                                  void *tm);
  efi_status_t (*set_wakeup_time)(bool enabled, void *tm);
  efi_status_t (*set_virtual_address_map)(unsigned long memory_map_size,
                                          unsigned long descriptor_size,
                                          uint32_t descriptor_version,
                                          efi_memory_desc_t *virtual_map);
  void					*convert_pointer;
  efi_status_t (*get_variable)(uint16_t *name, efi_guid_t *vendor, uint32_t *attr,
                               unsigned long *data_size, void *data);
  efi_status_t (*get_next_variable)(unsigned long *name_size, uint16_t *name,
                                    efi_guid_t *vendor);
  efi_status_t (*set_variable)(uint16_t *name, efi_guid_t *vendor,
                               uint32_t attr, unsigned long data_size,
                               void *data);
  efi_status_t (*get_next_high_mono_count)(uint32_t *count);
  efi_status_t (*reset_system)(int reset_type, efi_status_t status,
                               unsigned long data_size, uint16_t *data);

  efi_status_t (*update_capsule)(efi_capsule_header_t **capsules,
                                 unsigned long count,
                                 unsigned long sg_list);
  efi_status_t (*query_capsule_caps)(efi_capsule_header_t **capsules,
                                     unsigned long count,
                                     uint64_t *max_size,
                                     int *reset_type);
  efi_status_t (*query_variable_store)(uint32_t attributes,
                                       unsigned long size,
                                       bool nonblocking);
} efi_runtime_services_t;

typedef struct {

  efi_table_hdr_t hdr;
  void *raise_tpl;
  void *restore_tpl;
  efi_status_t (*allocate_pages)(int, int, unsigned long,
                                          efi_physical_addr_t *);
  efi_status_t (*free_pages)(efi_physical_addr_t,
                                      unsigned long);
  efi_status_t (*get_memory_map)(unsigned long *, void *,
                                          unsigned long *,
                                          unsigned long *, uint32_t *);
  efi_status_t (*allocate_pool)(int, unsigned long,
                                         void **);
  efi_status_t (*free_pool)(void *);
  efi_status_t (*create_event)(uint32_t, unsigned long,
                                        efi_event_notify_t, void *,
                                        efi_event_t *);
  efi_status_t (*set_timer)(efi_event_t,
                                     EFI_TIMER_DELAY, uint64_t);
  efi_status_t (*wait_for_event)(unsigned long,
                                          efi_event_t *,
                                          unsigned long *);
  void *signal_event;
  efi_status_t (*close_event)(efi_event_t);
  void *check_event;
  void *install_protocol_interface;
  void *reinstall_protocol_interface;
  void *uninstall_protocol_interface;
  efi_status_t (*handle_protocol)(efi_handle_t,
                                  efi_guid_t *, void **);
  void *__reserved;
  void *register_protocol_notify;
  efi_status_t (*locate_handle)(int, efi_guid_t *,
                                         void *, unsigned long *,
                                         efi_handle_t *);
  efi_status_t (*locate_device_path)(efi_guid_t *,
                                              efi_device_path_protocol_t **,
                                              efi_handle_t *);
  efi_status_t (*install_configuration_table)(efi_guid_t *,
                                                       void *);
  void *load_image;
  void *start_image;
  efi_status_t (*exit)(efi_handle_t,
                                           efi_status_t,
                                           unsigned long,
                       const uint16_t *);
  void *unload_image;
  efi_status_t (*exit_boot_services)(efi_handle_t,
                                              unsigned long);
  void *get_next_monotonic_count;
  efi_status_t (*stall)(unsigned long);
  void *set_watchdog_timer;
  void *connect_controller;
  efi_status_t (*disconnect_controller)(efi_handle_t,
                                                 efi_handle_t,
                                                 efi_handle_t);
  void *open_protocol;
  void *close_protocol;
  void *open_protocol_information;
  void *protocols_per_handle;
  void *locate_handle_buffer;
  efi_status_t (*locate_protocol)(efi_guid_t *, void *,
                                           void **);
  void *install_multiple_protocol_interfaces;
  void *uninstall_multiple_protocol_interfaces;
  void *calculate_crc32;
  void *copy_mem;
  void *set_mem;
  void *create_event_ex;
} efi_boot_services_t;


typedef struct efi_simple_text_output_protocol {
  void *reset;
  long (*output_string)(struct efi_simple_text_output_protocol *p,
                        uint16_t *char16);
  void *test_string;
} efi_simple_text_output_protocol_t;



typedef struct {
  efi_table_hdr_t hdr;
  const uint16_t *fw_vendor;
  uint32_t fw_revision;

  uint64_t con_in_handle;
  void *con_in; // efi_simple_text_input_protocol_t *con_in;

  uint64_t con_out_handle;
  efi_simple_text_output_protocol_t *con_out;

  uint64_t stderr_handle;
  uint64_t stderr;
  const efi_runtime_services_t *runtime;
  const efi_boot_services_t *boot;
  uint64_t nr_tables;
  efi_config_table_t *tables;
} efi_system_table_t;



typedef struct {
  uint32_t		revision;
  efi_handle_t		parent_handle;
  efi_system_table_t	*system_table;
  efi_handle_t		device_handle;
  void			*file_path;
  void			*reserved;
  uint32_t		load_options_size;
  void			*load_options;
  void			*image_base;
  uint64_t		image_size;
  unsigned int		image_code_type;
  unsigned int		image_data_type;
  efi_status_t		(*unload)(efi_handle_t image_handle);
} efi_loaded_image_t;



typedef struct efi_image_handle {
  efi_system_table_t system_table;
  efi_boot_services_t boot_services;
  efi_runtime_services_t runtime_services;
  efi_config_table_t config_tables[1];
  efi_loaded_image_t loaded_image;
  uint16_t fw_vendor[5];
} efi_image_handle_t;




extern void efi_init_runtime_services(efi_image_handle_t *h);

efi_status_t efi_boot_exit_boot_services_prepare(efi_handle_t H,
                                                 unsigned long mapkey);
