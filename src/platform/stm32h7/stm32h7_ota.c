#include <mios/pushpull.h>
#include <mios/ota.h>
#include <mios/elf.h>
#include <mios/block.h>
#include <mios/fs.h>
#include <mios/eventlog.h>
#include <mios/service.h>

#include <net/service/svc_ota.h>


static block_iface_t *g_upgrade_partition;

static volatile uint16_t *const FLASH_SIZE = (volatile uint16_t *)0x1FF1E880;

void
ota_partition_spiflash(block_iface_t *flash)
{
  if(flash == NULL)
    return;

  const uint32_t flashsize_kb = *FLASH_SIZE; // in kB

  // First 128kb is one int-flash-sector (where the bootloader lives)
  // So subtract 128kb but add one spi-flash-block for metadata
  size_t system_flash_blocks = (flashsize_kb - 128) / 4 + 1;

  g_upgrade_partition =
    block_create_partition(flash, 0, system_flash_blocks,
                           BLOCK_PARTITION_AUTOLOCK);

  block_iface_t *fs =
    block_create_partition(flash,
                           system_flash_blocks,
                           flash->num_blocks - system_flash_blocks, 0);

  fs_init(fs);
  eventlog_to_fs(100000);
}


struct stream *
ota_get_stream(void)
{
  if(g_upgrade_partition == NULL)
    return NULL;

  // Start at block 1 on SPI-flash, Main mios image begins at paddr 0x8020000
  return elf_to_bin(bin_to_ota(g_upgrade_partition, 1), 0x8020000);
}


#ifdef ENABLE_NET_DSIG

static error_t
stm32h7_ota_open(pushpull_t *pp)
{
  if(g_upgrade_partition == NULL)
    return ERR_NO_DEVICE;

  error_t err = ota_prohibit_upgrade();
  if(err)
    return err;

  return ota_open_with_args(pp, g_upgrade_partition,
                            128, // Skip over first 128kB in xfer
                            4,   // Offset 4kB on partition
                            0,   // Automatic blocksize
                            NULL);
}

SERVICE_DEF_PUSHPULL("ota", 0, 0, stm32h7_ota_open);

#endif

__attribute__((weak))
error_t ota_prohibit_upgrade(void)
{
  return 0;
}
