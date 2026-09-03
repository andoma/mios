#include <mios/pushpull.h>
#include <mios/service.h>
#include <mios/block.h>
#include <mios/error.h>

#include <net/service/svc_ota.h>

#include "host_ota.h"

// Set by the OTA test suite once it has built a virtual-SPI-NOR upgrade
// partition. NULL in a normal host run.
static block_iface_t *g_upgrade_partition;

void
host_ota_set_partition(block_iface_t *partition)
{
  g_upgrade_partition = partition;
}

static error_t
host_ota_open(pushpull_t *pp)
{
  if(g_upgrade_partition == NULL)
    return ERR_NO_DEVICE;

  // The host "image" is the ELF itself (no execute-in-place reshape), so
  // nothing is skipped in the transfer. Block 0 of the partition holds the
  // OTA header; the image is written from 4 kB in, mirroring the STM32
  // platforms so the same svc_ota logic and header layout are exercised.
  return ota_open_with_args(pp, g_upgrade_partition,
                            0,   // xfer_skip_kb  (send the whole image)
                            4,   // writeout_skip_kb (image at 4 kB)
                            0,   // blocksize (auto)
                            NULL);
}

SERVICE_DEF_PUSHPULL("ota", 0, 0, host_ota_open);
