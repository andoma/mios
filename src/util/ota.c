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
  void *sa_opaque;
  service_event_cb_t *sa_cb;
  pbuf_t *sa_info;
  int sa_shutdown;

  thread_t *sa_thread;
  mutex_t sa_mutex;
  cond_t sa_cond;
  pbuf_t *sa_rxbuf;
} svc_ota_t;

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
  sa->sa_cb(sa->sa_opaque, SERVICE_EVENT_WAKEUP);
  return pb;
}


static void
ota_send_final_status(svc_ota_t *sa, uint8_t status)
{
  pbuf_t *reply = pbuf_make(0,0);
  if(reply) {
    pbuf_write(reply, &status, 1, 10);
    mutex_lock(&sa->sa_mutex);
    sa->sa_info = reply;
    mutex_unlock(&sa->sa_mutex);
    sa->sa_cb(sa->sa_opaque, SERVICE_EVENT_WAKEUP);
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

  int first_sector = -1;
  int num_sectors = 0;
  size_t consecutive_size = 0;

  int num_blocks;
  uint32_t crc;

  memcpy(&num_blocks, pbuf_cdata(pb, 0), 4);
  memcpy(&crc, pbuf_cdata(pb, 4), 4);
  pbuf_free(pb);

  const size_t size = num_blocks * OTA_BLOCKSIZE;

  const flash_iface_t *fif = flash_get_primary();

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

  evlog(LOG_NOTICE,
        "OTA: %d bytes available for upgrade buffer (need %d) CRC:%08x",
        consecutive_size, size, crc);

  if(consecutive_size < size)
    return ERR_NO_FLASH_SPACE;

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

  crc = ~crc_acc;
  if(crc != ~crc_acc)
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
  sa->sa_cb(sa->sa_opaque, SERVICE_EVENT_CLOSE);
  thread_exit(NULL);
}


static void *
ota_open(void *opaque, service_event_cb_t *cb, size_t max_fragment_size)
{
  if(ota_busy)
    return NULL;

  svc_ota_t *sa = xalloc(sizeof(svc_ota_t), 0, MEM_MAY_FAIL);
  if(sa == NULL)
    return NULL;
  memset(sa, 0, sizeof(svc_ota_t));
  sa->sa_opaque = opaque;
  sa->sa_cb = cb;

  pbuf_t *pb = pbuf_make(0, 0);
  if(pb != NULL) {
    uint8_t hdr[4] = { 0, 's', 32, 0 };
    pb = pbuf_write(pb, hdr, sizeof(hdr), max_fragment_size);
    pb = pbuf_write(pb, mios_build_id(), 20, max_fragment_size);
    const char *appname = mios_get_app_name();
    pb = pbuf_write(pb, appname, strlen(appname), max_fragment_size);
  }

  if(pb == NULL) {
    free(sa);
    return NULL;
  }
  ota_busy = 1;
  sa->sa_info = pb;
  mutex_init(&sa->sa_mutex, "ota");
  cond_init(&sa->sa_cond, "ota");
  sa->sa_thread = thread_create(ota_thread, sa, 512, "ota", 0, 2);
  return sa;
}


static struct pbuf *
ota_pull(void *opaque)
{
  svc_ota_t *sa = opaque;
  pbuf_t *pb = sa->sa_info;
  sa->sa_info = NULL;
  return pb;
}

static pbuf_t *
ota_push(void *opaque, struct pbuf *pb)
{
  svc_ota_t *sa = opaque;
  mutex_lock(&sa->sa_mutex);
  sa->sa_rxbuf = pb;
  cond_signal(&sa->sa_cond);
  mutex_unlock(&sa->sa_mutex);
  return NULL;
}


static int
ota_may_push(void *opaque)
{
  svc_ota_t *sa = opaque;
  return sa->sa_rxbuf == NULL;
}

static void
ota_close(void *opaque)
{
  svc_ota_t *sa = opaque;

  mutex_lock(&sa->sa_mutex);
  sa->sa_shutdown = 1;
  cond_signal(&sa->sa_cond);
  mutex_unlock(&sa->sa_mutex);
  thread_join(sa->sa_thread);
  pbuf_free(sa->sa_info);
  free(sa);
  ota_busy = 0;
}

SERVICE_DEF("ota",
            ota_open, ota_push, ota_may_push, ota_pull, ota_close);
