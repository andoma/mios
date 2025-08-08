#include "stm32h7_ota.h"

#include <mios/pushpull.h>
#include <mios/block.h>

#include <net/service/svc_ota.h>


error_t
stm32h7_ota_open(pushpull_t *pp, block_iface_t *bi)
{
  if(bi == NULL)
    return ERR_NO_DEVICE;

  return ota_open_with_args(pp, bi,
                            128, // Skip over first 128kB in xfer
                            4,   // Offset 4kB on partition
                            0,   // Automatic blocksize
                            NULL);
}

