#include "stm32f4_ota.h"

#include <mios/ota.h>
#include <mios/elf.h>
#include <mios/block.h>
#include <mios/fs.h>
#include <mios/eventlog.h>

#include <stdio.h>

static block_iface_t *g_upgrade_partition;

void
ota_partition_spiflash(block_iface_t *flash)
{
  if(flash == NULL)
    return;

  static volatile uint16_t *const INT_FLASH_SIZE =
    (volatile uint16_t *)0x1fff7a22;

  const uint32_t int_flash_size = *INT_FLASH_SIZE * 1024;
  size_t system_flash_blocks = int_flash_size / flash->block_size;

  printf("stm32f4: Using %d blocks (%d kB) for upgrade partition\n",
         system_flash_blocks,
         system_flash_blocks * flash->block_size / 1024);

  g_upgrade_partition =
    block_create_partition(flash, 0, system_flash_blocks,
                           BLOCK_PARTITION_AUTOLOCK);

  block_iface_t *fs =
    block_create_partition(flash, system_flash_blocks,
                           flash->num_blocks - system_flash_blocks, 0);

  fs_init(fs);
  eventlog_to_fs(100000);
}


struct stream *
ota_get_stream(void)
{
  if(g_upgrade_partition == NULL)
    return NULL;

  // Start at block 4 on SPI-flash, Main mios image begins at paddr 0x8004000
  return elf_to_bin(bin_to_ota(g_upgrade_partition, 4), 0x8004000);
}


__attribute__((weak))
error_t ota_prohibit_upgrade(void)
{
  return 0;
}
