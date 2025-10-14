#include "smbios.h"

#include <mios/dmi.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>

struct smbios_hdr {
  uint8_t type;
  uint8_t length;
  uint16_t handle;
} __attribute__((packed));

struct type0 {
  struct smbios_hdr h;
  uint8_t vendor;
  uint8_t version;
  uint16_t start_segment;
  uint8_t release_date;
  uint8_t rom_size;
  uint64_t characteristics;
  uint8_t ext_char1;
  uint8_t ext_char2;
} __attribute__((packed));

struct type1 {
  struct smbios_hdr h;
  uint8_t manufacturer;
  uint8_t product_name;
  uint8_t version;
  uint8_t serial;
  uint8_t uuid[16];
  uint8_t wakeup_type;
  uint8_t sku;
  uint8_t family;
} __attribute__((packed));


struct type2 {
  struct smbios_hdr h;
  uint8_t manufacturer;
  uint8_t product;
  uint8_t version;
  uint8_t serial;
  uint8_t asset_tag;
  uint8_t feature_flags;
  uint8_t loc_in_chassis;
  uint16_t chassis_handle;
  uint8_t board_type;
  uint8_t obj_handles;
} __attribute__((packed));

struct type3 {
  struct smbios_hdr h;
  uint8_t manufacturer;
  uint8_t chassis_type;
  uint8_t version;
  uint8_t serial;
  uint8_t asset_tag;
  uint8_t bootup_state;
  uint8_t psu_state;
  uint8_t thermal_state;
  uint8_t security;
  uint32_t oem_defined;
  uint8_t height_u;
  uint8_t power_cords;
  uint8_t elem_count;
  uint8_t elem_record_len;
} __attribute__((packed));

struct type127 {
  struct smbios_hdr h;
};

struct smbios3_entry {
  uint8_t anchor[5]; /* "_SM3_" */
  uint8_t checksum;
  uint8_t length;
  uint8_t major;
  uint8_t minor;
  uint8_t docrev;
  uint8_t ep_revision;
  uint8_t reserved;
  uint32_t max_struct_size;
  uint64_t table_address;
} __attribute__((packed));


static uint8_t
sum8(const void *p, size_t n)
{
  const uint8_t *b = p;
  uint32_t s = 0;
  for(size_t i = 0; i < n; i++)
    s += b[i];
  return (uint8_t)s;
}

__attribute__((noinline))
static int
makestr(void **pp, int *cnt, const char *str)
{
  if(str == NULL)
    return 0;

  size_t len = strlen(str);
  char *dst = *pp;
  memcpy(dst, str, len);
  dst[len] = 0;
  *pp = dst + len + 1;
  int r = *cnt;
  *cnt = r + 1;
  return r;
}


__attribute__((weak))
const char *
dmi_get_str(int id)
{
  return NULL;
}


void
build_and_install_smbios3(void *buffer, size_t maxlen)
{
  int cnt;
  memset(buffer, 0, maxlen);
  struct smbios3_entry *ep = buffer;

  buffer += sizeof(struct smbios3_entry);
  void *p = buffer;

  // Type 0 (BIOS)

  cnt = 1;
  struct type0 *t0 = p;
  p += sizeof(struct type0);
  t0->h.type = 0;
  t0->h.length = sizeof(struct type0);
  t0->rom_size = 0xFF;
  t0->vendor = makestr(&p, &cnt, dmi_get_str(DMI_BIOS_VENDOR));
  t0->version = makestr(&p, &cnt, dmi_get_str(DMI_BIOS_VERSION));
  t0->release_date = makestr(&p, &cnt, dmi_get_str(DMI_BIOS_RELEASE_DATE));
  p++;

  // Type 1 (System / Product)

  cnt = 1;
  struct type1 *t1 = p;
  p += sizeof(struct type1);
  t1->h.type = 1;
  t1->h.length = sizeof(struct type1);
  t1->h.handle = 0x0100;

  t1->manufacturer = makestr(&p, &cnt, dmi_get_str(DMI_PRODUCT_MANUFACTURER));
  t1->product_name = makestr(&p, &cnt, dmi_get_str(DMI_PRODUCT_PRODUCT_NAME));
  t1->version = makestr(&p, &cnt, dmi_get_str(DMI_PRODUCT_VERSION));
  t1->serial = makestr(&p, &cnt, dmi_get_str(DMI_PRODUCT_SERIAL));
  memset(t1->uuid, 0, 16);
  t1->wakeup_type = 2; // Unknown
  t1->sku = makestr(&p, &cnt, dmi_get_str(DMI_PRODUCT_SKU));
  t1->family = makestr(&p, &cnt, dmi_get_str(DMI_PRODUCT_FAMILY));
  p++;

  // Type 3 (Chassi)

  cnt = 1;
  struct type3 *t3 = p;
  p += sizeof(struct type3);
  t3->h.type = 3;
  t3->h.length = sizeof(struct type3);
  t3->h.handle = 0x0300;
  t3->manufacturer = makestr(&p, &cnt, dmi_get_str(DMI_CHASSI_MANUFACTURER));
  t3->chassis_type = 0x02; // Unknown
  t3->version = makestr(&p, &cnt, dmi_get_str(DMI_CHASSI_VERSION));
  t3->serial = makestr(&p, &cnt, dmi_get_str(DMI_CHASSI_SERIAL));
  t3->asset_tag = makestr(&p, &cnt, dmi_get_str(DMI_CHASSI_ASSET_TAG));
  t3->bootup_state = 0x03;
  t3->psu_state = 0x03;
  t3->thermal_state = 0x03; /* = “Safe” */
  t3->security = 0x03;      /* None */
  t3->oem_defined = 0;
  t3->height_u = 0;
  t3->power_cords = 0;
  t3->elem_count = 0;
  t3->elem_record_len = 0;
  p++;

  // Type 2 (Baseboard / Mobo)

  cnt = 1;
  struct type2 *t2 = p;
  p += sizeof(struct type2);
  t2->h.type = 2;
  t2->h.length = sizeof(struct type2);
  t2->h.handle = 0x0200;
  t2->manufacturer = makestr(&p, &cnt, dmi_get_str(DMI_BB_MANUFACTURER));
  t2->product = makestr(&p, &cnt, dmi_get_str(DMI_BB_PRODUCT));
  t2->version = makestr(&p, &cnt, dmi_get_str(DMI_BB_VERSION));
  t2->serial = makestr(&p, &cnt, dmi_get_str(DMI_BB_SERIAL));
  t2->asset_tag = makestr(&p, &cnt, dmi_get_str(DMI_BB_ASSET_TAG));
  t2->feature_flags = 0x01; /* hosting board */
  t2->loc_in_chassis = 6;
  t2->chassis_handle = t3->h.handle;
  t2->board_type = 0x0A; /* Motherboard */
  t2->obj_handles = 0;
  p++;

  struct type127 *t127 = p;
  p += sizeof(struct type127);
  t127->h.type = 127;
  t127->h.length = sizeof(struct type127);
  t127->h.handle = 0xffff;

  memcpy(ep->anchor, "_SM3_", 5);
  ep->length = 0x18;
  ep->major = 3;
  ep->minor = 8;
  ep->ep_revision = 1;
  ep->max_struct_size = p - buffer;
  ep->table_address = (uintptr_t)buffer;

  uint8_t sum = sum8(ep, ep->length);
  ep->checksum = (uint8_t)(0x100 - sum);
}
