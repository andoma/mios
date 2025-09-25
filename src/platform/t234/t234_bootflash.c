#include "t234_bootinfo.h"

#include <mios/gpt.h>
#include <mios/block.h>
#include <mios/type_macros.h>
#include <mios/eventlog.h>

#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#include <sys/param.h>

#include "lib/crypto/sha512.h"
#include "util/crc32.h"

typedef struct {
  const char *name;
  int start_lba;
  int rcmblobtype;
} partition_info_t;


static const partition_info_t default_partition_table[] = {
  {"BCT",                        0},
  {"A_mb1",                   2048, 0x81}, // < These three are present in the
  {"A_psc_bl1",               3072, 0x82}, // < br_BCT and must be aligned to
  {"A_MB1_BCT",               3584, 0x83}, // < 512 sector boundary
  {"A_MEM_BCT",               3840, 0x84},
  {"A_tsec-fw",               4352, 0x13},
  {"A_nvdec",                 6400, 0x07},
  {"A_mb2",                   8448, 0x06},
  {"A_xusb-fw",               9472, 0x24},
  {"A_bpmp-fw",               9984, 0x0f},
  {"A_bpmp-fw-dtb",          13056, 0x10},
  {"A_psc-fw",               21248, 0x11},
  {"A_mts-mce",              22784, 0x08},
  {"A_sc7",                  23808, 0x85},
  {"A_pscrf",                24192, 0x86},
  {"A_mb2rf",                24576, 0x87},
  {"A_cpu-bootloader",       24832, 0x2b},
  {"A_secure-os",            32000, 0x27},
  {"A_smm-fw",               40192, 0x00},
  {"A_eks",                  44288, 0x29},
  {"A_dce-fw",               44800, 0x1f},
  {"A_spe-fw",               55040, 0x25},
  {"A_rce-fw",               56192, 0x23},
  {"A_adsp-fw",              58240, 0x21},
  {"A_pva-fw",               62336, 0x3b},
  {"A_reserved_on_boot",     62848},
  {"B_mb1",                  65024, 0x81}, // < These three are present in the
  {"B_psc_bl1",              66048, 0x82}, // < br_BCT and must be aligned to
  {"B_MB1_BCT",              66560, 0x83}, // < 512 sector boundary
  {"B_MEM_BCT",              66816, 0x84},
  {"B_tsec-fw",              67328, 0x13},
  {"B_nvdec",                69376, 0x07},
  {"B_mb2",                  71424, 0x06},
  {"B_xusb-fw",              72448, 0x24},
  {"B_bpmp-fw",              72960, 0x0f},
  {"B_bpmp-fw-dtb",          76032, 0x10},
  {"B_psc-fw",               84224, 0x11},
  {"B_mts-mce",              85760, 0x08},
  {"B_sc7",                  86784, 0x85},
  {"B_pscrf",                87168, 0x86},
  {"B_mb2rf",                87552, 0x87},
  {"B_cpu-bootloader",       87808, 0x2b},
  {"B_secure-os",            94976, 0x27},
  {"B_smm-fw",              103168, 0x00},
  {"B_eks",                 107264, 0x29},
  {"B_dce-fw",              107776, 0x1f},
  {"B_spe-fw",              118016, 0x25},
  {"B_rce-fw",              119168, 0x23},
  {"B_adsp-fw",             121216, 0x21},
  {"B_pva-fw",              125312, 0x3b},
  {"B_reserved_on_boot",    125824},
  {"uefi_variables",        128000},
  {"uefi_ftw",              128512},
  {"reserved",              129536},
  {"worm",                  129920},
  {"BCT-boot-chain_backup", 130304},
  {"reserved_partition",    130432},
  {"secondary_gpt_backup",  130560},
  {"B_VER",                 130688},
  {"A_VER",                 130816},
  {NULL,                    130944},
};


static error_t
copy_to_qspi(block_iface_t *b, int block, const void *data, size_t len,
             const char *name)
{
  error_t err;
  void *buf;

  size_t checklen = MIN(8192, len);

  buf = xalloc(checklen, 0, MEM_MAY_FAIL);
  if(buf == NULL)
    return ERR_NO_MEMORY;

  err = block_read(b, block, 0, buf, checklen);
  if(err)
    return err;

  int header_already_there = !memcmp(data, buf, checklen);
  free(buf);
  if(header_already_there) {
    evlog(LOG_INFO, "%s: Already installed at %d", name, block);
    return 0;
  }

  const int blocks = (len + b->block_size - 1) / b->block_size;

  evlog(LOG_INFO, "%s: Erase block %d +%d", name, block, blocks);
  err = block_erase(b, block, blocks);
  if(err)
    return err;

  evlog(LOG_INFO, "%s: Writing %zd bytes to block %d +%d",
        name, len, block, blocks);

  while(len) {
    size_t to_copy = MIN(len, b->block_size);
    err = block_write(b, block, 0, data, to_copy);
    if(err)
      return err;

    len -= to_copy;
    data += to_copy;
    block++;
  }
  evlog(LOG_INFO, "%s: Done", name);
  return 0;
}



static error_t
install_from_rcmblob(block_iface_t *bi, const struct rcmblob_header *rbh)
{
  error_t err = 0;

  for(size_t i = 0; i < ARRAYSIZE(default_partition_table); i++) {
    const partition_info_t *pi = &default_partition_table[i];
    if(pi->rcmblobtype == 0)
      continue;
    for(size_t j = 0; j < rbh->num_items; j++) {
      if(pi->rcmblobtype != rbh->items[j].type)
        continue;

      const int start = pi->start_lba;
      const int length = pi[1].start_lba - start;
      const int size = length * 512;

      if(rbh->items[j].length > size) {
        evlog(LOG_INFO, "%s: RCM item size %u > Max:%u",
              pi->name, rbh->items[j].length, size);
        continue;
      }

      err = copy_to_qspi(bi, start,
                         (void *)rbh + rbh->items[j].location,
                         rbh->items[j].length,
                         pi->name);
      if(err)
        break;
    }
  }

  return err;
}


static const uint8_t basic_data_partition_uuid[16] = {
  0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
  0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};

#define gpt_crc32(ptr, len) crc32(0, (void *)(ptr), len)


static error_t
write_gpt(block_iface_t *bi, struct efi_header *hdr,
          struct efi_entry *tbl,
          uint32_t hdr_sector, uint32_t tbl_sector)
{
  error_t err;

  hdr->entries_lba = tbl_sector;

  evlog(LOG_INFO, "Installing GPT at %d, entries at %d",
        hdr_sector, tbl_sector);

  hdr->crc32 = 0;
  hdr->crc32 = gpt_crc32(hdr, sizeof(struct efi_header));

  err = copy_to_qspi(bi, hdr->entries_lba, tbl,
                     hdr->entries_count * hdr->entries_size,
                     "GPT-entries");
  if(err)
    return err;

  return copy_to_qspi(bi, hdr_sector, hdr, 512, "GPT-header");
}

static error_t
install_gpt(block_iface_t *bi)
{
  const int num_entries = 128;
  struct efi_entry *tbl = xalloc(sizeof(struct efi_entry) * num_entries,
                                 0, MEM_CLEAR | MEM_MAY_FAIL);
  if(tbl == NULL)
    return ERR_NO_MEMORY;

  struct efi_header *hdr = xalloc(512, 0, MEM_CLEAR | MEM_MAY_FAIL);

  if(hdr == NULL) {
    free(tbl);
    return ERR_NO_MEMORY;
  }

  for(size_t i = 0; i < ARRAYSIZE(default_partition_table); i++) {
    const partition_info_t *pi = &default_partition_table[i];
    const char *name = pi->name;
    if(name == NULL)
      continue;

    struct efi_entry *ee = tbl + i;

    memcpy(ee->type_uuid, basic_data_partition_uuid, sizeof(ee->type_uuid));
    for(size_t j = 0; j < sizeof(ee->uniq_uuid); j++) {
      ee->uniq_uuid[j] = rand();
    }

    ee->first_lba = pi[0].start_lba;
    ee->last_lba  = pi[1].start_lba - 1;
    ee->attr = 0;
    const size_t namelen = MIN(strlen(name), EFI_NAMELEN - 1);
    for(size_t j = 0; j < namelen; j++) {
      ee->name[j] = name[j];
    }
  }

  memcpy(hdr->magic, EFI_MAGIC, 8);
  hdr->version = EFI_VERSION;
  hdr->header_sz = 0x5c;
  hdr->header_lba = 131071;
  hdr->backup_lba = 1;
  hdr->first_lba = 0;
  hdr->last_lba = 131038;

  for(size_t j = 0; j < sizeof(hdr->volume_uuid); j++) {
    hdr->volume_uuid[j] = rand();
  }

  hdr->entries_count = num_entries;
  hdr->entries_size = sizeof(struct efi_entry);
  hdr->entries_crc32 = gpt_crc32(tbl, hdr->entries_size * num_entries);
  error_t err;

  err = write_gpt(bi, hdr, tbl, 131071, 131039);
  if(!err) {
    err = write_gpt(bi, hdr, tbl, 130592, 130560);
  }
  free(tbl);
  free(hdr);
  return err;
}
static error_t
install_bct(block_iface_t *bi, int chain, int just_one)
{
  char name[8];
  uint8_t *buf = xalloc(8192, 0, MEM_MAY_FAIL | MEM_CLEAR);
  if(buf == NULL)
    return ERR_NO_MEMORY;

  memcpy(buf, "BCTB", 4);
  memcpy(buf + 0x1210, "BCTB", 4);
  buf[0x1248] = 0x4;   // Offset to A_mb1 / 512
  buf[0x1258] = 0x6;   // Offset to A_psc_bl1 / 512
  buf[0x1268] = 0x7;   // Offset to A_MB1_BCT / 512
  buf[0x1278] = 0x7f;  // Offset to B_mb1 / 512
  buf[0x1288] = 0x81;  // Offset to B_psc_bl1 / 512
  buf[0x1298] = 0x82;  // Offset to B_MB1_BCT / 512

  buf[0x19dc] = 2;     // Number of boot chains
  buf[0x19d8] = chain; // Current chain

  SHA512(buf + 6848, buf + 5900, 948);
  SHA512(buf + 0x1c4, buf + 4608, 3584);
  SHA512(buf + 4, buf + 0x44, 0x1fbc);

  error_t err = 0;
  for(int i = 0; i < 64 && !err; i++) {
    snprintf(name, sizeof(name), "BCT-%d", i);
    err = copy_to_qspi(bi, i * 32, buf, 8192, name);
    if(just_one)
      break;
  }

  free(buf);
  return err;
}

error_t
t234_bootflash_install(block_iface_t *bi)
{
  error_t err;

  const struct rcmblob_header *rbh = get_carveout_base(CARVEOUT_RCM_BLOB);
  if(rbh == NULL)
    return ERR_NOT_FOUND;

  int64_t t0 = clock_get();

  err = install_from_rcmblob(bi, rbh);
  if(err)
    return err;

  err = install_gpt(bi);
  if(err)
    return err;

  err = install_bct(bi, 0, 0);
  if(err)
    return err;

  err = block_ctrl(bi, BLOCK_SYNC);
  if(err)
    return err;

  int64_t t1 = clock_get();
  evlog(LOG_INFO, "install complete (%d seconds)", (int)((t1 - t0) / 1000000));
  return 0;
}


error_t
t234_bootflash_set_chain(struct block_iface *bi, int chain)
{
  error_t err = install_bct(bi, chain, 1);
  if(err)
    return err;

  return block_ctrl(bi, BLOCK_SYNC);
}
