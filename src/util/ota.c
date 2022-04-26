#include <mios/ota.h>

#include <net/pbuf.h>

#include <stdio.h>
#include <net/mbus/mbus.h>
#include <net/socket.h>

#include <mios/rpc.h>
#include <mios/flash.h>

#include <stdlib.h>
#include <unistd.h>

#include "util/crc32.h"


typedef struct {
  uint32_t blocks;
  uint32_t crc;
  pbuf_t *in;
  int16_t first_sector;
  int16_t num_sectors;
  uint32_t next_block;
  socket_t sock;
  cond_t cond;

  uint16_t cur_sector;
  uint32_t cur_offset;
  uint32_t cur_sector_size;

  uint32_t crc_acc;

} ota_state_t;

static mutex_t ota_mutex = MUTEX_INITIALIZER("ota");

static ota_state_t *ota_state;


pbuf_t *
mbus_ota_upgrade(struct mbus_netif *ni, pbuf_t *pb, uint8_t src_addr)
{
  mutex_lock(&ota_mutex);
  ota_state_t *os = ota_state;
  if(os != NULL) {
    pbuf_t *old = os->in;
    ota_state->in = pb;
    pb = old;
    cond_signal(&os->cond);
  }
  mutex_unlock(&ota_mutex);
  return pb;
}


static void
ota_xfer_xmit(ota_state_t *os)
{
  const uint8_t pkt[4] = {MBUS_OP_OTA_XFER,
                          os->next_block,
                          os->next_block >> 8,
                          os->next_block >> 16};

  socket_send(&os->sock, pkt, sizeof(pkt), 0);
}

static void
ota_xfer_done(ota_state_t *os, error_t error)
{
  const uint8_t pkt[5] = {MBUS_OP_OTA_XFER, 0xff,0xff,0xff, -error};

  socket_send(&os->sock, pkt, sizeof(pkt), 0);
}


static int
ota_xfer_recv(ota_state_t *os, pbuf_t *pb, const flash_iface_t *fif)
{
  const size_t blocksize = 16;

  if((pb = pbuf_pullup(pb, pb->pb_pktlen)) == NULL)
    return -1;

  const uint8_t *u8 = pbuf_data(pb, 0);
  const uint32_t block = u8[1] | (u8[2] << 8) | (u8[3] << 16);

  if(pb->pb_pktlen != blocksize + 4) {
    pbuf_free(pb);
    return -1;
  }

  u8 += 4;

  int r = -1;
  if(os->next_block == block) {
    fif->write(fif, os->cur_sector, os->cur_offset, u8, blocksize);

    const void *mem = fif->get_addr(fif, os->cur_sector) + os->cur_offset;
    os->crc_acc = crc32(os->crc_acc, mem, blocksize);

    os->next_block++;
    os->cur_offset += blocksize;

    if(os->cur_offset == os->cur_sector_size) {
      os->cur_sector++;
      os->cur_offset = 0;
      os->cur_sector_size = fif->get_sector_size(fif, os->cur_sector);
    }
    r = 0;
  }

  pbuf_free(pb);
  return r;
}


__attribute__((noreturn))
static void *
ota_task(void *arg)
{
  ota_state_t *os = arg;

  error_t err = socket_attach(&os->sock);
  if(err) {
    ota_xfer_done(os, err);
  } else {

    for(int i = 1; i <= 10; i++) {

      printf("OTA: Erasing sector %d - %d\n", os->first_sector,
             os->first_sector + os->num_sectors - 1);
      usleep(20000);

      const flash_iface_t *fif = flash_get_primary();
      for(int i = 0; i < os->num_sectors; i++) {
        fif->erase_sector(fif, os->first_sector + i);
      }

      printf("OTA: Ready for transfer, attempt #%d\n", i);

      mutex_lock(&ota_mutex);

      os->cur_sector = os->first_sector;
      os->cur_offset = 0;
      os->cur_sector_size = fif->get_sector_size(fif, os->cur_sector);
      os->crc_acc = 0;
      os->next_block = 0;

      ota_xfer_xmit(os);

      while(os->next_block != os->blocks) {

        pbuf_t *pb = os->in;
        if(pb) {
          os->in = NULL;
          mutex_unlock(&ota_mutex);
          int r = ota_xfer_recv(os, pb, fif);
          mutex_lock(&ota_mutex);
          if(!r) {
            ota_xfer_xmit(os);
            continue;
          }
        }

        if(cond_wait_timeout(&os->cond, &ota_mutex, clock_get() + 100000)) {
          ota_xfer_xmit(os);
        }
      }

      mutex_unlock(&ota_mutex);

      uint32_t crc = ~os->crc_acc;

      if(crc == os->crc) {
        printf("OTA: Transfer complete. CRC:%x\n", crc);
        for(int i = 0; i < 5; i++) {
          ota_xfer_done(os, 0);
          usleep(25000);
        }

        const void *src = fif->get_addr(fif, os->first_sector);
        fif->multi_write(fif, src, src, FLASH_MULTI_WRITE_CPU_REBOOT);
        reboot();
      }
      printf("OTA: CRC MISMATCH. Computed:%08x Expected:%08x\n",
             crc, os->crc);
    }
    socket_detach(&os->sock);
    ota_xfer_done(os, ERR_CHECKSUM_ERROR);
  }
  mutex_lock(&ota_mutex);
  ota_state = NULL;
  free(os);
  mutex_unlock(&ota_mutex);
  task_exit(NULL);
}


__attribute__((weak))
error_t
rpc_ota(const ota_req_t *in, void *out, size_t in_size)
{
  const flash_iface_t *fif = flash_get_primary();
  int first_sector = -1;
  int num_sectors = 0;
  size_t consecutive_size = 0;

  if(in->type != OTA_TYPE_SECTIONS)
    return ERR_MISMATCH;

  mutex_lock(&ota_mutex);

  if(ota_state) {
    mutex_unlock(&ota_mutex);
    return ERR_NOT_READY;
  }

  const size_t size = in->blocks * 16;

  for(int i = 0; ; i++) {
    int type = fif->get_sector_type(fif, i);
    if(type == 0)
      break;

    if(type == FLASH_SECTOR_TYPE_AVAIL) {
      if(first_sector == -1)
        first_sector = i;
      consecutive_size += fif->get_sector_size(fif, i);
      num_sectors++;
      if(consecutive_size >= size)
        break;
    } else {
      first_sector = -1;
      num_sectors = 0;
      consecutive_size = 0;
    }
  }

  printf("OTA: %d bytes available for upgrade buffer (need %d) CRC:%08x\n",
         consecutive_size, size, in->crc);
  if(consecutive_size < size) {
    mutex_unlock(&ota_mutex);
    return ERR_NO_FLASH_SPACE;
  }

  ota_state_t *os = calloc(1, sizeof(ota_state_t));
  cond_init(&os->cond, "ota");
  os->blocks = in->blocks;
  os->crc = in->crc;
  os->first_sector = first_sector;
  os->num_sectors = num_sectors;

  socket_init(&os->sock, AF_MBUS, 0);
  os->sock.s_remote_addr = in->hostaddr;

  task_t *t = task_create(ota_task, os, 768, "ota", TASK_DETACHED, 3);
  if(t == NULL) {
    free(os);
    mutex_unlock(&ota_mutex);
    return ERR_NO_BUFFER;
  }
  ota_state = os;
  mutex_unlock(&ota_mutex);
  return 0;
}



RPC_DEF("ota", sizeof(ota_req_t), 0, rpc_ota, 0);

__attribute__((weak))
error_t
rpc_otamode(const void *in, uint8_t *out, size_t in_size)
{
  out[0] = OTA_TYPE_SECTIONS;
  return 0;
}

RPC_DEF("otamode", 0, 1, rpc_otamode, 0);
