#include "net/pbuf.h"

#include <mios/error.h>
#include <mios/mios.h>

#include <string.h>

#include "stm32g0_clk.h"

#include <stdio.h>
#include <unistd.h>


void  __attribute__((weak))
ota_platform_info(uint8_t *info)
{
  info[0] = 0;
  info[1] = 'r';
  info[2] = 32;
  info[3] = 2;
}

error_t  __attribute__((weak))
ota_platform_start(uint32_t flow_header, struct pbuf *pb)
{
  if(pb->pb_pktlen < 8)
    return ERR_MALFORMED;

  fini();

  stm32g0_stop_pll(); // Bootloader expects to be run at 16MHz

  uint32_t blocks;
  uint32_t crc;
  memcpy(&blocks, pbuf_cdata(pb, 0), 4);
  memcpy(&crc, pbuf_cdata(pb, 4), 4);

  void **ota_startp = (void *)0x08000010;

  void (*ota_start)(uint32_t flow_header, uint32_t num_blocks, uint32_t image_crc) = *ota_startp;

  ota_start(flow_header, blocks, crc);
  return 0;
}
