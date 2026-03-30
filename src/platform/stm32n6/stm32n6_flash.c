#include "stm32n6_flash.h"

#include <mios/fs.h>
#include <mios/copy.h>

#include <stdio.h>
#include <string.h>

stream_t *block_write_stream_create(block_iface_t *bi);

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
  int slot = app_slot_from_url(url + 4); // Skip "app:" prefix
  if(slot < 0)
    return NULL;
  return block_write_stream_create(flash_partitions[slot]);
}


COPY_HANDLER_DEF(app, 5,
  .prefix = "app:",
  .open_write = app_copy_open_write,
);
