#include "mbus.h"

#include "util/crc32.h"

#include "net/pbuf.h"
#include "net/netif.h"

#include <stdio.h>

static uint32_t
mbus_crc32(struct pbuf *pb)
{
  uint32_t crc = 0;

  for(; pb != NULL; pb = pb->pb_next)
    crc = crc32(crc, pb->pb_data + pb->pb_offset, pb->pb_buflen);

  return ~crc;
}


static void
mbus_output(mbus_netif_t *mni, struct pbuf *pb, uint8_t dst_addr)
{
  pb = pbuf_prepend(pb, mni->mni_hdr_len);

  uint8_t *hdr = pbuf_data(pb, 0);

  const uint8_t addr = (dst_addr & 0xf) | (mni->mni_ni.ni_local_addr << 4);

  switch(mni->mni_hdr_len) {
  case 1:
    hdr[0] = addr;
    break;
  default:
    pbuf_free(pb);
    return;
  }

  uint32_t crc = mbus_crc32(pb);
  uint32_t *trailer = pbuf_append(pb, sizeof(uint32_t));
  *trailer = crc;
  mni->mni_output(mni, pb);
}


struct pbuf *
mbus_local(mbus_netif_t *mni, pbuf_t *pb, uint8_t src_addr)
{
  uint8_t *pkt = pbuf_data(pb, 0);
  uint8_t opcode = pkt[0] & 0xf;

  switch(opcode) {
  case MBUS_OP_PING:
    pkt[0] = MBUS_OP_PONG;
    mbus_output(mni, pb, src_addr);
    return NULL;

  default:
    return pb;
  }
}


struct pbuf *
mbus_input(struct netif *ni, struct pbuf *pb)
{
  mbus_netif_t *mni = (mbus_netif_t *)ni;

  if(mbus_crc32(pb)) {
    printf("mbus: bad crc\n");
    return pb;
  }

  if((pb = pbuf_trim(pb, 4)) == NULL) {
    printf("mbus: short packet\n");
    return pb;
  }

  if((pb = pbuf_pullup(pb, mni->mni_hdr_len + 1)) == NULL) {
    printf("mbus: short packet\n");
    return pb;
  }

  const uint8_t *hdr = pbuf_data(pb, 0);
  uint8_t addr = hdr[0];
  pb = pbuf_drop(pb, mni->mni_hdr_len);

  uint8_t dst_addr = addr & 0xf;

  if(ni->ni_local_addr == dst_addr) {
    // Destined for us
    return mbus_local(mni, pb, addr >> 4);
  }
  // FIXME: Add routing
  return pb;
}


void
mbus_netif_attach(mbus_netif_t *mni, const char *name, uint8_t addr)
{
  mni->mni_ni.ni_local_addr = addr;
  mni->mni_ni.ni_input = mbus_input;
  netif_attach(&mni->mni_ni);
}
