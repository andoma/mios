#include <mios/pushpull.h>
#include <mios/ota.h>
#include <mios/block.h>
#include <mios/fs.h>
#include <mios/eventlog.h>
#include <mios/service.h>

#include <net/service/svc_ota.h>

static block_iface_t *g_upgrade_partition;

static volatile uint16_t *const FLASH_SIZE = (volatile uint16_t *)0x1fff75e0;

void
ota_partition_spiflash(block_iface_t *flash)
{
  if(flash == NULL)
    return;

  const uint32_t flashsize_kb = *FLASH_SIZE; // in kB
  size_t system_flash_blocks = flashsize_kb / 4;

  g_upgrade_partition =
    block_create_partition(flash, 0, system_flash_blocks,
                           BLOCK_PARTITION_AUTOLOCK);

  block_iface_t *fs =
    block_create_partition(flash,
                           system_flash_blocks,
                           flash->num_blocks - system_flash_blocks, 0);
#if 0
  fs = block_create_verifier(fs,
                             BLOCK_VERIFIER_PANIC_ON_ERR
                             /* | BLOCK_VERIFIER_DUMP */);
#endif
  fs_init(fs);
  eventlog_to_fs(100000);
}


#ifdef ENABLE_NET_DSIG

static error_t
stm32g4_ota_open(pushpull_t *pp)
{
  if(g_upgrade_partition == NULL)
    return ERR_NO_DEVICE;

  error_t err = ota_prohibit_upgrade();
  if(err)
    return err;

  // stm32g4_bootloader.c hardcodes its SPI-flash image read offset at
  // 2048 bytes (header + image share the partition's first 4kB
  // sector), and stm32g4_bootloader.ld gives the app a 2048-byte BOOT
  // region ahead of it -- so both the wire transfer and the partition
  // write need to skip exactly that same 2kB.
  return ota_open_with_args(pp, g_upgrade_partition,
                            2,  // Skip over first 2kB (BOOT region) in xfer
                            2,  // Offset 2kB on partition
                            0,  // Automatic blocksize
                            NULL);
}

SERVICE_DEF_PUSHPULL("ota", 0, 0, stm32g4_ota_open);

#endif

__attribute__((weak))
error_t ota_prohibit_upgrade(void)
{
  return 0;
}
