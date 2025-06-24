#include "efi.h"
#include <stddef.h>

#define NV_ADDRESS_MAP_AON_HSP_BASE  0x0c150000
#define NV_ADDRESS_MAP_TOP0_HSP_BASE 0x03c00000

__attribute__((always_inline,unused))
static void
outc(uint8_t c)
{
  volatile uint32_t *ptr = (uint32_t *)0x0c168000;
  while(*ptr) {}
  *ptr = (1 << 31) | (1 << 24) | c;
  while(*ptr) {}
}

__attribute__((always_inline,unused))
static uint8_t
getc(void)
{
  volatile uint32_t *ptr = (uint32_t *)0x03c10000;
  while(1) {
    uint32_t c = *ptr;
    if(!(c & (1 << 31)))
      continue;
    *ptr = 0;
    return c;
  }
}

__attribute__((always_inline,unused))
static void
out64(uint64_t val)
{
  for(int i = 60; i >= 0; i -= 4) {
    uint64_t n = (val >> i) & 0xf;
    outc(n > 9 ? n + 'a' - 10 : n + '0');
  }
}


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


__attribute__((always_inline))
static inline void
flush_all_dcache_to_pou(void)
{
    uint64_t clidr;

    /* Discover how many cache levels the core implements */
    asm volatile ("mrs %0, clidr_el1" : "=r"(clidr));

    for (int level = 0; level < 7; level++) {          /* max 7 levels EL1 */
        unsigned ctype = (clidr >> (level * 3)) & 0x7; /* 0=No cache, 1=I, 2=D, 3=U */

        if (ctype < 2)                 /* skip if I‑only or no cache      */
            continue;                  /* we need Data (2) or Unified (3) */

        /* ----------- point CSSELR at <level, data/unified> ------------ */
        asm volatile ("msr csselr_el1, %0\n"
                      "isb"            :: "r"((uint64_t)(level << 1)));

        uint64_t ccsidr;
        asm volatile ("mrs %0, ccsidr_el1" : "=r"(ccsidr));

        unsigned line_len_log2 = (ccsidr & 0x7) + 4;          /* log2(bytes/line)  */
        unsigned num_ways      = ((ccsidr >>  3) & 0x3ff) + 1;
        unsigned num_sets      = ((ccsidr >> 13) & 0x7fff) + 1;

        unsigned way_shift = __builtin_clz(num_ways - 1);

        /* ------------- sweep every <set, way> in this level ------------ */
        for (int set = num_sets - 1; set >= 0; set--) {
            for (int way = num_ways - 1; way >= 0; way--) {
                uint64_t setway =
                    ((uint64_t)level << 1) |                      /* 2 LSBs = level */
                    ((uint64_t)way  << way_shift) |
                    ((uint64_t)set  << line_len_log2);

                asm volatile ("dc cisw, %0" :: "r"(setway));
            }
        }
    }

    /* Make sure every clean/invalid is complete & visible */
    asm volatile ("dsb sy");
    asm volatile ("isb");
}


__attribute__((always_inline,unused))
static inline void
disable_mmu_icache_dcache(void)
{
    unsigned long sctlr;

    /* Read current control bits */
    asm volatile ("mrs %0, sctlr_el1" : "=r" (sctlr));

    /* Clear I-cache, D-cache, and MMU enables */
    sctlr &= ~((1UL << 12) |   /* I */
               (1UL <<  2) |   /* C */
               (1UL <<  0));   /* M */

    /* Write it back */
    asm volatile ("isb");
    asm volatile ("msr sctlr_el1, %0" :: "r" (sctlr) : "memory");

    /* Ensure the new attributes are now in force */
    asm volatile ("isb");
}

__attribute__((always_inline))
static inline void
disable_mmu_dcache(void)
{
    unsigned long sctlr;

    /* Read current control bits */
    asm volatile ("mrs %0, sctlr_el1" : "=r" (sctlr));

    sctlr &= ~((1UL <<  2) |   /* C */
               (1UL <<  0));   /* M */

    /* Write it back */
    asm volatile ("msr sctlr_el1, %0" :: "r" (sctlr) : "memory");

    /* Ensure the new attributes are now in force */
    asm volatile ("isb");
}

__attribute__((always_inline))
static inline void
disable_icache(void)
{
    unsigned long sctlr;

    /* Read current control bits */
    asm volatile ("mrs %0, sctlr_el1" : "=r" (sctlr));

    sctlr &= ~((1UL << 12) |   /* I */
               0);
    /* Write it back */
    asm volatile ("ic iallu");
    asm volatile ("dsb sy" ::: "memory");
    asm volatile ("msr sctlr_el1, %0" :: "r" (sctlr) : "memory");
    asm volatile ("isb");
}

__attribute__((section("efi_runtime"),noinline))
static void
wat(void)
{
  asm volatile ("msr daifset, #2\n"
                "isb\n");

  outc('1');

  asm volatile ("dsb sy" ::: "memory");
  flush_all_dcache_to_pou();   /* your own loop over DC CISW/DC CIVAC */
  outc('2');

  // disable_icache();
  outc('3');

  //  disable_mmu_icache_dcache(); /* step 2 above */
  outc('4');

  asm volatile ("tlbi vmalle1\n\t"
                "dsb sy\n\t"
                "isb");
  outc('5');

  //  disable_icache();
  outc('6');

  asm volatile ("hvc #0\n" ::: "memory");

  for(int i = 0; i < 100; i++)
    outc('7');
}

__attribute__((section("efi_runtime"),noinline))
static efi_status_t
efi_exit_boot_services(efi_handle_t H,
                       unsigned long mapkey)
{
  return 0;
  wat();
  outc('\n');
  outc('\n');
  //  void *ret = __builtin_return_address(0);
  out64((unsigned long)0);
  outc('\n');
  while(1) {
    int c = getc();
    if(c == 'q')
      break;
    outc(c);
  }
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
