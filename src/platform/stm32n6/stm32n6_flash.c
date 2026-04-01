#include "stm32n6_reg.h"
#include "stm32n6_flash.h"
#include "stm32n6_bootstatus.h"

#include <mios/fs.h>
#include <mios/copy.h>
#include <mios/ota.h>
#include <mios/eventlog.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

block_iface_t *xspi_norflash_create(void);

/*
 * Flash partition layout (4KB blocks):
 *
 * Offset      Size    Blocks      Contents
 * 0x000000    256KB   0-63        FSBL1 (bootloader A)
 * 0x040000    256KB   64-127      FSBL2 (bootloader B)
 * 0x080000    4KB     128         Boot selector
 * 0x081000    508KB   129-255     Reserved
 * 0x100000    2MB     256-767     Application A
 * 0x300000    2MB     768-1279    Application B
 * 0x500000    rest    1280-end    Filesystem (LittleFS)
 */

#define FLASH_BLOCK_SIZE     0x1000  // 4KB

#define FSBL1_OFFSET         0x000000
#define FSBL2_OFFSET         0x040000
#define BOOTSELECTOR_OFFSET  0x080000
#define APP_A_OFFSET         0x100000
#define APP_B_OFFSET         0x300000
#define FILESYSTEM_OFFSET    0x500000

#define OFFSET_TO_BLOCK(x)   ((x) / FLASH_BLOCK_SIZE)
#define SIZE_TO_BLOCKS(x)    ((x) / FLASH_BLOCK_SIZE)

static block_iface_t *flash_partitions[FLASH_PARTITION_COUNT];

block_iface_t *
stm32n6_flash_get_partition(int partition)
{
  if(partition < 0 || partition >= FLASH_PARTITION_COUNT)
    return NULL;
  return flash_partitions[partition];
}

static void __attribute__((constructor(5100)))
stm32n6_flash_init(void)
{
  block_iface_t *flash = xspi_norflash_create();
  if(flash == NULL) {
    printf("stm32n6_flash: XSPI init failed\n");
    return;
  }

  const size_t total_blocks = flash->num_blocks;
  const size_t fs_start = OFFSET_TO_BLOCK(FILESYSTEM_OFFSET);

  if(total_blocks <= fs_start) {
    printf("stm32n6_flash: Flash too small for partition layout\n");
    return;
  }

  flash_partitions[FLASH_PARTITION_FSBL1] =
    block_create_partition(flash,
                           OFFSET_TO_BLOCK(FSBL1_OFFSET),
                           OFFSET_TO_BLOCK(FSBL2_OFFSET) - OFFSET_TO_BLOCK(FSBL1_OFFSET),
                           BLOCK_PARTITION_AUTOLOCK);

  flash_partitions[FLASH_PARTITION_FSBL2] =
    block_create_partition(flash,
                           OFFSET_TO_BLOCK(FSBL2_OFFSET),
                           OFFSET_TO_BLOCK(BOOTSELECTOR_OFFSET) - OFFSET_TO_BLOCK(FSBL2_OFFSET),
                           BLOCK_PARTITION_AUTOLOCK);

  flash_partitions[FLASH_PARTITION_BOOTSELECTOR] =
    block_create_partition(flash,
                           OFFSET_TO_BLOCK(BOOTSELECTOR_OFFSET),
                           1,
                           BLOCK_PARTITION_AUTOLOCK);

  flash_partitions[FLASH_PARTITION_APP_A] =
    block_create_partition(flash,
                           OFFSET_TO_BLOCK(APP_A_OFFSET),
                           SIZE_TO_BLOCKS(APP_B_OFFSET - APP_A_OFFSET),
                           BLOCK_PARTITION_AUTOLOCK);

  flash_partitions[FLASH_PARTITION_APP_B] =
    block_create_partition(flash,
                           OFFSET_TO_BLOCK(APP_B_OFFSET),
                           SIZE_TO_BLOCKS(FILESYSTEM_OFFSET - APP_B_OFFSET),
                           BLOCK_PARTITION_AUTOLOCK);

  flash_partitions[FLASH_PARTITION_FILESYSTEM] =
    block_create_partition(flash,
                           fs_start,
                           total_blocks - fs_start,
                           BLOCK_PARTITION_AUTOLOCK);

  fs_init(flash_partitions[FLASH_PARTITION_FILESYSTEM]);
}


// =====================================================================
// Application partition write stream
//
// On-flash format:
//   [magic "mIAP" (4)] [length LE32 (4)] [image data] [~crc32 (4)] ["mI0sIMG1" (8)]
//
// The 8-byte header is only on flash, never in the streamed data.
// Block 0 is buffered in RAM and written last (with the header filled in).
// CRC trailer is checked/appended on flush.
// =====================================================================

#include <util/crc32.h>

#define MIOS_APP_MAGIC    0x5041496d  // "mIAP" little-endian
#define MIOS_IMG_MAGIC    "mI0sIMG1"
#define MIOS_IMG_TRAILER  12          // 4 (crc) + 8 (magic)
#define APP_HEADER_SIZE   8           // 4 (magic) + 4 (length)

typedef struct {
  stream_t stream;
  block_iface_t *bi;
  uint32_t crc;          // CRC of committed data (excludes tail)
  uint32_t image_pos;    // Bytes committed to flash/block0 so far
  uint8_t *block0;       // First block buffer (block_size bytes)
  uint8_t target_slot;   // 'A' or 'B'
  uint8_t switch_boot;   // Also switch boot selector on success
  uint8_t tail_len;
  uint8_t tail[MIOS_IMG_TRAILER];
} app_write_stream_t;


// Write committed image data to flash (or block0 buffer).
// Data goes to flash at offset (image_pos + APP_HEADER_SIZE).
// Block 0 is deferred in RAM; blocks 1+ are erased-on-first-touch.
static error_t
aws_commit(app_write_stream_t *aws, const void *data, size_t len)
{
  const uint8_t *src = data;
  const size_t bs = aws->bi->block_size;

  while(len > 0) {
    size_t flash_off = aws->image_pos + APP_HEADER_SIZE;
    size_t block = flash_off / bs;
    size_t off_in_block = flash_off % bs;
    size_t chunk = bs - off_in_block;
    if(chunk > len)
      chunk = len;

    if(block == 0) {
      memcpy(aws->block0 + flash_off, src, chunk);
    } else {
      if(off_in_block == 0) {
        error_t err = block_erase(aws->bi, block, 1);
        if(err) return err;
      }
      error_t err = block_write(aws->bi, block, off_in_block, src, chunk);
      if(err) return err;
    }

    src += chunk;
    len -= chunk;
    aws->image_pos += chunk;
  }
  return 0;
}

// Commit data from the tail/input and update CRC
static error_t
aws_commit_with_crc(app_write_stream_t *aws, const void *data, size_t len)
{
  aws->crc = crc32(aws->crc, data, len);
  return aws_commit(aws, data, len);
}


static ssize_t
aws_write(stream_t *s, const void *data, size_t size, int flags)
{
  app_write_stream_t *aws = (app_write_stream_t *)s;

  if(data == NULL) {
    // Flush — handle trailer, write header, finalize block 0

    // Check for existing trailer in buffered tail
    if(aws->tail_len == MIOS_IMG_TRAILER &&
       !memcmp(aws->tail + 4, MIOS_IMG_MAGIC, 8)) {
      // Existing trailer — verify CRC
      if(~crc32(aws->crc, aws->tail, 4) != 0) {
        evlog(LOG_ERR, "app: image CRC mismatch");
        return ERR_CHECKSUM_ERROR;
      }
      // Commit the validated trailer
      error_t err = aws_commit(aws, aws->tail, MIOS_IMG_TRAILER);
      if(err) return err;
    } else {
      // No valid trailer — flush tail data and append one
      evlog(LOG_WARNING, "app: image has no checksum, adding one");

      if(aws->tail_len > 0) {
        error_t err = aws_commit_with_crc(aws, aws->tail, aws->tail_len);
        if(err) return err;
      }

      uint32_t stored_crc = ~aws->crc;
      error_t err = aws_commit(aws, &stored_crc, 4);
      if(err) return err;
      err = aws_commit(aws, MIOS_IMG_MAGIC, 8);
      if(err) return err;
    }

    // Fill in the partition header in block 0
    uint32_t magic = MIOS_APP_MAGIC;
    uint32_t length = aws->image_pos;
    memcpy(aws->block0 + 0, &magic, 4);
    memcpy(aws->block0 + 4, &length, 4);

    // Erase and write block 0
    error_t err = block_erase(aws->bi, 0, 1);
    if(err) return err;
    err = block_write(aws->bi, 0, 0, aws->block0,
                      aws->bi->block_size);
    if(err) return err;

    // Clear dirty bit for target slot
    uint32_t bs = reg_rd(BSEC_SCRATCH0);
    if(aws->target_slot == 'B')
      bs &= ~BOOTSTATUS_APP_B_DIRTY;
    else
      bs &= ~BOOTSTATUS_APP_A_DIRTY;
    reg_wr(BSEC_SCRATCH0, bs);

    if(aws->switch_boot) {
      // Switch boot selector to target slot
      block_iface_t *sel = flash_partitions[FLASH_PARTITION_BOOTSELECTOR];
      err = block_erase(sel, 0, 1);
      if(err) return err;
      err = block_write(sel, 0, 0, &aws->target_slot, 1);
      if(err) return err;
    }

    evlog(LOG_INFO, "app: written %d bytes to slot %c%s",
          (int)length, aws->target_slot,
          aws->switch_boot ? " (boot switched)" : "");
    return 0;
  }

  // Buffer the last MIOS_IMG_TRAILER bytes, commit everything else.
  const uint8_t *src = data;
  size_t total = aws->tail_len + size;

  if(total <= MIOS_IMG_TRAILER) {
    memcpy(aws->tail + aws->tail_len, src, size);
    aws->tail_len = total;
    return size;
  }

  // Commit (total - MIOS_IMG_TRAILER) bytes, keep 12 in tail
  size_t to_flush = total - MIOS_IMG_TRAILER;

  // First commit from existing tail
  size_t from_tail = to_flush < aws->tail_len ? to_flush : aws->tail_len;
  if(from_tail > 0) {
    error_t err = aws_commit_with_crc(aws, aws->tail, from_tail);
    if(err) return err;
  }

  // Then commit from new data
  size_t from_data = to_flush - from_tail;
  if(from_data > 0) {
    error_t err = aws_commit_with_crc(aws, src, from_data);
    if(err) return err;
  }

  // Build new tail: remaining old tail bytes + remaining new data
  size_t old_tail_remaining = aws->tail_len - from_tail;
  if(old_tail_remaining > 0)
    memmove(aws->tail, aws->tail + from_tail, old_tail_remaining);
  memcpy(aws->tail + old_tail_remaining, src + from_data, size - from_data);
  aws->tail_len = MIOS_IMG_TRAILER;

  return size;
}

static void
aws_close(stream_t *s)
{
  app_write_stream_t *aws = (app_write_stream_t *)s;
  free(aws->block0);
  free(aws);
}

static const stream_vtable_t app_write_stream_vtable = {
  .write = aws_write,
  .close = aws_close,
};


static stream_t *
app_write_stream_open(block_iface_t *bi, uint8_t target_slot, int switch_boot)
{
  if(bi == NULL)
    return NULL;

  app_write_stream_t *aws = xalloc(sizeof(*aws), 0, MEM_MAY_FAIL | MEM_CLEAR);
  if(aws == NULL)
    return NULL;

  aws->block0 = xalloc(bi->block_size, 0, MEM_MAY_FAIL);
  if(aws->block0 == NULL) {
    free(aws);
    return NULL;
  }
  memset(aws->block0, 0xff, bi->block_size);

  aws->stream.vtable = &app_write_stream_vtable;
  aws->bi = bi;
  aws->target_slot = target_slot;
  aws->switch_boot = switch_boot;

  return &aws->stream;
}


// =====================================================================
// Copy handler for "app:" protocol
// =====================================================================

static int
app_slot_from_url(const char *url)
{
  if(!strcmp(url, "a"))
    return FLASH_PARTITION_APP_A;
  if(!strcmp(url, "b"))
    return FLASH_PARTITION_APP_B;
  return -1;
}

static stream_t *
app_copy_open_write(const char *url)
{
  const char *slot_str = url + 4; // Skip "app:" prefix
  int slot = app_slot_from_url(slot_str);
  if(slot < 0)
    return NULL;

  uint8_t target = (*slot_str | 0x20) == 'b' ? 'B' : 'A';
  return app_write_stream_open(flash_partitions[slot], target, 0);
}

COPY_HANDLER_DEF(app, 5,
  .prefix = "app:",
  .open_write = app_copy_open_write,
);


// =====================================================================
// Copy handler for "bootloader:" protocol
// Accepts ELF, extracts .boot section, generates FSBL header, writes
// to FSBL1 (and optionally FSBL2) partition.
// =====================================================================

#include <mios/elf.h>

// FSBL header (UM3234 Table 32) — 160 bytes
struct stm32n6_fsbl_header {
  uint8_t  magic[4];            // 'S','T','M',0x32
  uint8_t  signature[96];
  uint32_t image_checksum;
  uint32_t header_version;      // 0x00020300
  uint32_t image_length;
  uint32_t entry_point;
  uint32_t reserved1;
  uint32_t load_address;        // 0xFFFFFFFF
  uint32_t reserved2;
  uint32_t version_number;
  uint32_t extension_flags;     // 0x80000000 (padding ext)
  uint32_t post_header_length;
  uint32_t binary_type;         // 0x10
  uint8_t  pad[8];
  uint32_t ns_payload_length;
  uint32_t ns_payload_hash;
} __attribute__((packed));

// Padding extension header
struct stm32n6_padding_ext {
  uint8_t  type[4];             // 'S','T',0xFF,0xFF
  uint32_t length;
} __attribute__((packed));

// Binary must start at offset 0x400 in flash so it lands at SRAM
// 0x34180400 (matching the linker address). The boot ROM loads
// header+extensions+binary contiguously to 0x34180000.
#define FSBL_HEADER_TOTAL 0x400

typedef struct {
  stream_t stream;
  block_iface_t *bi;
  uint32_t checksum;
  uint32_t offset;       // Current write offset within partition (after header)
  uint8_t erased;
} bootloader_write_stream_t;


static ssize_t
blws_write(stream_t *s, const void *data, size_t size, int flags)
{
  bootloader_write_stream_t *bws = (bootloader_write_stream_t *)s;
  if(data == NULL) {
    // Flush — pad to 32-byte alignment and write FSBL header

    // Pad final bytes to 32-byte alignment
    uint32_t image_len = bws->offset;
    if(image_len & 31) {
      uint8_t pad[32] = {};
      size_t pad_len = 32 - (image_len & 31);
      error_t err = block_write(bws->bi, 0, FSBL_HEADER_TOTAL + image_len,
                                pad, pad_len);
      if(err) return err;
      for(size_t i = 0; i < pad_len; i++)
        bws->checksum += 0; // zeros don't change sum
      image_len += pad_len;
    }

    // Build FSBL header
    struct stm32n6_fsbl_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0] = 'S';
    hdr.magic[1] = 'T';
    hdr.magic[2] = 'M';
    hdr.magic[3] = 0x32;
    hdr.header_version = 0x00020300;
    hdr.image_checksum = bws->checksum;
    hdr.image_length = image_len;
    hdr.entry_point = 0x34180401;
    hdr.load_address = 0xFFFFFFFF;
    hdr.extension_flags = 0x80000000;
    hdr.post_header_length = FSBL_HEADER_TOTAL - sizeof(struct stm32n6_fsbl_header);
    hdr.binary_type = 0x10;

    // Padding extension
    struct stm32n6_padding_ext pad_ext;
    pad_ext.type[0] = 'S';
    pad_ext.type[1] = 'T';
    pad_ext.type[2] = 0xFF;
    pad_ext.type[3] = 0xFF;
    pad_ext.length = FSBL_HEADER_TOTAL - sizeof(struct stm32n6_fsbl_header);

    // Write padding extension and header into the already-erased first block
    error_t err = block_write(bws->bi, 0, sizeof(hdr), &pad_ext, sizeof(pad_ext));
    if(err) return err;

    err = block_write(bws->bi, 0, 0, &hdr, sizeof(hdr));
    if(err) return err;

    printf("Bootloader: %d bytes, checksum 0x%x\n",
           (int)image_len, (unsigned)bws->checksum);
    return 0;
  }

  // Erase entire partition on first data
  if(!bws->erased) {
    error_t err = block_erase(bws->bi, 0, bws->bi->num_blocks);
    if(err) return err;
    bws->erased = 1;
  }

  // Update checksum
  const uint8_t *src = data;
  for(size_t i = 0; i < size; i++)
    bws->checksum += src[i];

  // Write data at current offset (after header area)
  error_t err = block_write(bws->bi, 0, FSBL_HEADER_TOTAL + bws->offset,
                            data, size);
  if(err) return err;

  bws->offset += size;
  return size;
}

static void
blws_close(stream_t *s)
{
  free(s);
}

static const stream_vtable_t bootloader_write_vtable = {
  .write = blws_write,
  .close = blws_close,
};


static stream_t *
bootloader_copy_open_write(const char *url)
{
  // bootloader:a → FSBL1, bootloader:b → FSBL2
  const char *slot_str = url + 11; // Skip "bootloader:"
  int partition;
  if(!strcmp(slot_str, "a"))
    partition = FLASH_PARTITION_FSBL1;
  else if(!strcmp(slot_str, "b"))
    partition = FLASH_PARTITION_FSBL2;
  else
    return NULL;

  block_iface_t *bi = flash_partitions[partition];
  if(bi == NULL)
    return NULL;

  bootloader_write_stream_t *bws = xalloc(sizeof(*bws), 0,
                                          MEM_MAY_FAIL | MEM_CLEAR);
  if(bws == NULL)
    return NULL;

  bws->stream.vtable = &bootloader_write_vtable;
  bws->bi = bi;

  // Wrap with elf_to_bin to extract only the .boot section
  // paddr range: 0x34180400 to 0x34200000
  return elf_to_bin(&bws->stream, 0x34180400, 0x34200000);
}


COPY_HANDLER_DEF(bootloader, 5,
  .prefix = "bootloader:",
  .open_write = bootloader_copy_open_write,
);


// =====================================================================
// Boot selector: 'A' or 'B' stored at byte 0 of the boot selector
// partition. 0xFF (erased) defaults to 'A'.
// =====================================================================

#include <mios/cli.h>

static error_t
cmd_bootslot(cli_t *cli, int argc, char **argv)
{
  block_iface_t *bi = flash_partitions[FLASH_PARTITION_BOOTSELECTOR];
  if(bi == NULL)
    return ERR_OPERATION_FAILED;

  if(argc == 1) {
    // Read current slot
    uint8_t sel;
    error_t err = block_read(bi, 0, 0, &sel, 1);
    if(err) return err;
    cli_printf(cli, "Boot slot: %c\n", sel == 'B' ? 'B' : 'A');
    return 0;
  }

  if(argc == 2 && (argv[1][0] == 'a' || argv[1][0] == 'A' ||
                    argv[1][0] == 'b' || argv[1][0] == 'B')) {
    uint8_t sel = (argv[1][0] | 0x20) == 'b' ? 'B' : 'A';
    error_t err = block_erase(bi, 0, 1);
    if(err) return err;
    err = block_write(bi, 0, 0, &sel, 1);
    if(err) return err;
    cli_printf(cli, "Boot slot set to %c\n", sel);
    return 0;
  }

  cli_printf(cli, "Usage: bootslot [a|b]\n");
  return ERR_INVALID_ARGS;
}

CLI_CMD_DEF("bootslot", cmd_bootslot);


// =====================================================================
// OTA: Write to inactive app slot, switch boot selector on success
// =====================================================================

struct stream *
ota_get_stream(void)
{
  uint32_t bs = reg_rd(BSEC_SCRATCH0);
  int booted_b = bs & BOOTSTATUS_BOOTED_B;

  int target_partition = booted_b ? FLASH_PARTITION_APP_A : FLASH_PARTITION_APP_B;
  uint8_t target_slot = booted_b ? 'A' : 'B';

  return app_write_stream_open(flash_partitions[target_partition], target_slot, 1);
}
