#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <unistd.h>

#include <mios/service.h>
#include <mios/version.h>
#include <mios/task.h>
#include <mios/flash.h>
#include <mios/eventlog.h>

#include "net/pbuf.h"

#include "util/crc32.h"
#include "irq.h"

#define OTA_BLOCKSIZE 32

typedef struct svc_ota {
  socket_t *sa_sock;

  pbuf_t *sa_info;
  int sa_shutdown;

  thread_t *sa_thread;
  mutex_t sa_mutex;
  cond_t sa_cond;
  pbuf_t *sa_rxbuf;
} svc_ota_t;


error_t  __attribute__((weak))
ota_platform_start(uint32_t flow_header, struct pbuf *pb)
{
  return 0;
}

void __attribute__((weak))
ota_platform_info(uint8_t *hdr)
{
  hdr[0] = 0;
  hdr[1] = 's';
  hdr[2] = 32;
  hdr[3] = 0;
}


static char ota_busy;



static pbuf_t *
ota_get_next_pkt(svc_ota_t *sa)
{
  pbuf_t *pb;
  mutex_lock(&sa->sa_mutex);
  while((pb = sa->sa_rxbuf) == NULL && !sa->sa_shutdown) {
    cond_wait(&sa->sa_cond, &sa->sa_mutex);
  }
  sa->sa_rxbuf = NULL;
  mutex_unlock(&sa->sa_mutex);
  sa->sa_sock->net->event(sa->sa_sock->net_opaque, SOCKET_EVENT_PUSH);
  return pb;
}


static void
ota_send_final_status(svc_ota_t *sa, uint8_t status)
{
  pbuf_t *reply = pbuf_make(0,0);
  if(reply) {
    reply = pbuf_write(reply, &status, 1, 10);
    mutex_lock(&sa->sa_mutex);
    sa->sa_info = reply;
    mutex_unlock(&sa->sa_mutex);
    sa->sa_sock->net->event(sa->sa_sock->net_opaque, SOCKET_EVENT_PULL);
  }
}

#include <stdio.h>

static error_t
ota_perform(svc_ota_t *sa)
{
  evlog(LOG_DEBUG, "OTA: Waiting for init");
  pbuf_t *pb = ota_get_next_pkt(sa);
  if(pb == NULL)
    return ERR_NOT_CONNECTED;

  if(pb->pb_pktlen != 8) {
    pbuf_free(pb);
    return ERR_BAD_PKT_SIZE;
  }


  int num_blocks;
  uint32_t crc;

  memcpy(&num_blocks, pbuf_cdata(pb, 0), 4);
  memcpy(&crc, pbuf_cdata(pb, 4), 4);
  pbuf_free(pb);

  const size_t size = num_blocks * OTA_BLOCKSIZE;

  const flash_iface_t *fif = flash_get_primary();

  // First, figure out number of sectors

  int total_sectors = 0;
  size_t total_size = 0;
  while(1) {
    size_t siz = fif->get_sector_size(fif, total_sectors);
    if(siz == 0)
      break;
    total_size += siz;
    total_sectors++;
  }

  evlog(LOG_NOTICE, "OTA: Flash total %d sectors (%d bytes)",
        total_sectors, total_size);

  if(total_sectors == 0)
    return ERR_NOSPC;

  int first_sector = -1;
  int last_sector = -1;
  size_t consecutive_size = 0;

  // Start from the end or we risk overwriting ourself
  for(int i = total_sectors - 1; i >= 0; i--) {
    const int type = fif->get_sector_type(fif, i);
    if(type == FLASH_SECTOR_TYPE_AVAIL) {
      if(last_sector == -1)
        last_sector = i;
      consecutive_size += fif->get_sector_size(fif, i);
      first_sector = i;
      if(consecutive_size >= size)
        break;
    } else {
      last_sector = -1;
      first_sector = -1;
      consecutive_size = 0;
    }
  }

  evlog(LOG_NOTICE,
        "OTA: %d bytes available for upgrade buffer (need %d) CRC:%08x",
        consecutive_size, size, crc);

  if(consecutive_size < size)
    return ERR_NOSPC;

  const int num_sectors = last_sector - first_sector + 1;
  for(int i = 0; i < num_sectors; i++) {
    evlog(LOG_DEBUG, "OTA: Erasing sector %d", first_sector + i);
    usleep(10000);
    error_t err = fif->erase_sector(fif, first_sector + i);
    evlog(LOG_DEBUG, "OTA: Erase sector %d -- %s", first_sector + i,
          error_to_string(err));
    if(err)
      return err;
    usleep(10000);
  }

  int cur_sector = first_sector;
  int cur_sector_size = fif->get_sector_size(fif, cur_sector);
  int cur_offset = 0;
  uint32_t crc_acc = 0;
  for(int i = 0; i < num_blocks; i++) {
    pbuf_t *pb = ota_get_next_pkt(sa);
    if(pb == NULL) {
      return ERR_NOT_CONNECTED;
    }
    error_t err =
      fif->write(fif, cur_sector, cur_offset, pbuf_cdata(pb, 0), OTA_BLOCKSIZE);
    pbuf_free(pb);
    if(err)
      return err;

    const void *mem = fif->get_addr(fif, cur_sector) + cur_offset;
    crc_acc = crc32(crc_acc, mem, OTA_BLOCKSIZE);

    cur_offset += OTA_BLOCKSIZE;

    if(cur_offset == cur_sector_size) {
      cur_sector++;
      cur_offset = 0;
      cur_sector_size = fif->get_sector_size(fif, cur_sector);
    }
  }

  crc_acc = ~crc_acc;

  evlog(LOG_DEBUG, "Computed CRC: 0x%08x  Expecting 0x%08x",
        crc_acc, crc);

  if(crc != crc_acc)
    return ERR_CHECKSUM_ERROR;

  ota_send_final_status(sa, 0);

  evlog(LOG_NOTICE, "OTA: Transfer OK");
  printf("\n\t*** Reboot due to OTA\n");
  usleep(50000);

  const void *src = fif->get_addr(fif, first_sector);

  irq_forbid(IRQ_LEVEL_ALL);
  fini();

  fif->multi_write(fif, src, src, FLASH_MULTI_WRITE_CPU_REBOOT);
  reboot();
  return 0;
}


__attribute__((noreturn))
static void *
ota_thread(void *arg)
{
  svc_ota_t *sa = arg;
  error_t err = ota_perform(sa);
  evlog(LOG_NOTICE, "OTA: Cancelled -- %s", error_to_string(err));
  ota_send_final_status(sa, -err);
  sa->sa_sock->net->event(sa->sa_sock->net_opaque, SOCKET_EVENT_CLOSE);
  thread_exit(NULL);
}


static struct pbuf *
ota_pull(void *opaque)
{
  svc_ota_t *sa = opaque;
  pbuf_t *pb = sa->sa_info;
  sa->sa_info = NULL;
  return pb;
}



static uint32_t
ota_push(void *opaque, struct pbuf *pb)
{
  svc_ota_t *sa = opaque;

  if(!sa->sa_thread) {

    uint32_t fh = sa->sa_sock->net->get_flow_header  ?
      sa->sa_sock->net->get_flow_header(sa->sa_sock->net_opaque) : 0;

    // This can take over the transfer and if so, it won't return
    error_t err = ota_platform_start(fh, pb);
    if(err) {
      pbuf_free(pb);
      return SOCKET_EVENT_PUSH;
    }

    sa->sa_thread = thread_create(ota_thread, sa, 512, "ota", 0, 2);
  }
  mutex_lock(&sa->sa_mutex);
  sa->sa_rxbuf = pb;
  cond_signal(&sa->sa_cond);
  mutex_unlock(&sa->sa_mutex);
  return 0;
}


static int
ota_may_push(void *opaque)
{
  svc_ota_t *sa = opaque;
  return sa->sa_rxbuf == NULL;
}

static void
ota_close(void *opaque, const char *reason)
{
  svc_ota_t *sa = opaque;

  mutex_lock(&sa->sa_mutex);
  sa->sa_shutdown = 1;
  cond_signal(&sa->sa_cond);
  mutex_unlock(&sa->sa_mutex);

  if(sa->sa_thread)
    thread_join(sa->sa_thread);
  else
    sa->sa_sock->net->event(sa->sa_sock->net_opaque, SOCKET_EVENT_CLOSE);

  pbuf_free(sa->sa_info);
  free(sa);
  ota_busy = 0;
}



static const socket_app_fn_t ota_fn = {
  .push = ota_push,
  .may_push = ota_may_push,
  .pull = ota_pull,
  .close = ota_close
};

static error_t
ota_open(socket_t *s)
{
  if(ota_busy)
    return ERR_NOT_READY;

  svc_ota_t *sa = xalloc(sizeof(svc_ota_t), 0, MEM_MAY_FAIL);
  if(sa == NULL)
    return ERR_NO_MEMORY;
  memset(sa, 0, sizeof(svc_ota_t));

  sa->sa_sock = s;
  s->app = &ota_fn;
  s->app_opaque = sa;

  pbuf_t *pb = pbuf_make(0, 0);
  if(pb != NULL) {
    uint8_t hdr[4];
    ota_platform_info(hdr);
    pb = pbuf_write(pb, hdr, sizeof(hdr), s->max_fragment_size);
    pb = pbuf_write(pb, mios_build_id(), 20, s->max_fragment_size);
    const char *appname = mios_get_app_name();
    pb = pbuf_write(pb, appname, strlen(appname), s->max_fragment_size);
  }

  if(pb == NULL) {
    free(sa);
    return ERR_NO_BUFFER;
  }
  ota_busy = 1;
  sa->sa_info = pb;
  mutex_init(&sa->sa_mutex, "ota");
  cond_init(&sa->sa_cond, "ota");
  return 0;
}

SERVICE_DEF("ota", 0, 9, SERVICE_TYPE_DGRAM, ota_open);
