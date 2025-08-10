#include "svc_ota.h"

#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/param.h>

#include <mios/service.h>
#include <mios/version.h>
#include <mios/task.h>
#include <mios/eventlog.h>
#include <mios/block.h>

#include "net/pbuf.h"

#include "util/crc32.h"
#include "irq.h"

static char ota_busy;

typedef struct {
  uint8_t magic[4];
  uint32_t size;
  uint32_t image_crc;
  uint32_t header_crc;
} otahdr_t;


typedef struct svc_ota {
  pushpull_t *sa_sock;

  pbuf_t *sa_info;
  uint8_t sa_blocksize;
  uint8_t sa_shutdown;
  uint8_t sa_skipped_kbytes;

  thread_t *sa_thread;
  mutex_t sa_mutex;
  cond_t sa_cond;
  pbuf_t *sa_rxbuf;

  otahdr_t sa_otahdr;

  block_iface_t *sa_partition;

  void (*sa_platform_upgrade)(uint32_t flow_header,
                              pbuf_t *pb);

} svc_ota_t;


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
  sa->sa_sock->net->event(sa->sa_sock->net_opaque, PUSHPULL_EVENT_PUSH);
  return pb;
}


static void
ota_send_final_status(svc_ota_t *sa, uint8_t status)
{
  pbuf_t *reply = pbuf_make(0,0);
  if(reply) {
    reply = pbuf_write(reply, &status, 1, sa->sa_sock);
    mutex_lock(&sa->sa_mutex);
    sa->sa_info = reply;
    mutex_unlock(&sa->sa_mutex);
    sa->sa_sock->net->event(sa->sa_sock->net_opaque, PUSHPULL_EVENT_PULL);
  }
}




static error_t
ota_perform(svc_ota_t *sa)
{
  evlog(LOG_DEBUG, "OTA: Waiting for init (blocksize: %d)", sa->sa_blocksize);
  pbuf_t *pb = ota_get_next_pkt(sa);
  if(pb == NULL)
    return ERR_NOT_CONNECTED;

  if(pb->pb_pktlen < 8) {
    pbuf_free(pb);
    return ERR_BAD_PKT_SIZE;
  }

  int num_blocks;
  uint32_t crc;

  memcpy(&num_blocks, pbuf_cdata(pb, 0), 4);
  memcpy(&crc, pbuf_cdata(pb, 4), 4);
  pb = pbuf_drop(pb, 8, 1);

  error_t err;

  block_iface_t *bi = sa->sa_partition;

  uint32_t total_bytes = num_blocks * sa->sa_blocksize;
  evlog(LOG_DEBUG, "OTA: Transfer started %d bytes", total_bytes);
  err = bi->erase(bi, 0);
  if(err)
    return err;

  uint32_t erased_sector = 0;
  uint32_t crc_acc = 0;
  uint32_t current_byte = 0;


  while(1) {

    if(current_byte >= total_bytes)
      break;

    if(pb == NULL) {
      pb = ota_get_next_pkt(sa);
      if(pb == NULL) {
        return ERR_NOT_CONNECTED;
      }
    }

    while(pb) {

      uint32_t byte_offset = (sa->sa_skipped_kbytes * 1024) + current_byte;
      uint32_t sector = byte_offset >> 12;
      uint32_t sector_offset = byte_offset & 4095;
      if(sector > erased_sector) {
        err = bi->erase(bi, sector);
        if(err) {
          pbuf_free(pb);
          return err;
        }
        erased_sector = sector;
      }

      void *buf = pbuf_data(pb, 0);
      size_t chunk_size = MIN(pb->pb_pktlen, 4096 - sector_offset);
      err = bi->write(bi, sector, sector_offset, buf, chunk_size);
      if(err) {
        pbuf_free(pb);
        return err;
      }
      // Readback
      err = bi->read(bi, sector, sector_offset, buf, chunk_size);
      if(err) {
        pbuf_free(pb);
        return err;
      }

      crc_acc = crc32(crc_acc, buf, chunk_size);
      pb = pbuf_drop(pb, chunk_size, 1);
      current_byte += chunk_size;
    }
  }

  crc_acc = ~crc_acc;

  evlog(LOG_DEBUG, "Computed CRC: 0x%08x  Expecting 0x%08x",
        crc_acc, crc);

  if(crc != crc_acc)
    return ERR_CHECKSUM_ERROR;

  memcpy(sa->sa_otahdr.magic, "OTA1", 4);
  sa->sa_otahdr.size = total_bytes;
  sa->sa_otahdr.image_crc = crc;
  sa->sa_otahdr.header_crc = ~crc32(0, &sa->sa_otahdr, sizeof(otahdr_t) - 4);

  err = bi->write(bi, 0, 0, &sa->sa_otahdr, sizeof(otahdr_t));
  if(err)
    return err;

  shutdown_notification("OTA");

  bi->ctrl(bi, BLOCK_SUSPEND);

  ota_send_final_status(sa, 0);
  evlog(LOG_NOTICE, "OTA: Transfer OK");
  printf("\n\t*** Reboot to OTA\n");
  usleep(100000);

  irq_forbid(IRQ_LEVEL_ALL);
  fini();
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
  sa->sa_sock->net->event(sa->sa_sock->net_opaque, PUSHPULL_EVENT_CLOSE);
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

    if(sa->sa_platform_upgrade && pb->pb_pktlen >= 8 &&
       sa->sa_sock->net->get_flow_header) {
      uint32_t fh = sa->sa_sock->net->get_flow_header(sa->sa_sock->net_opaque);
      sa->sa_platform_upgrade(fh, pb); // Will not return
      return 0;
    }

    sa->sa_thread = thread_create(ota_thread, sa, 0, "ota", 0, 2);
  }
  mutex_lock(&sa->sa_mutex);
  if(sa->sa_rxbuf == NULL) {
    sa->sa_rxbuf = pb;
  } else {
    void *dst = pbuf_append(sa->sa_rxbuf, pb->pb_pktlen);
    memcpy(dst, pbuf_data(pb, 0), pb->pb_pktlen);
    pbuf_free(pb);
  }
  cond_signal(&sa->sa_cond);
  mutex_unlock(&sa->sa_mutex);
  return 0;
}


static int
ota_may_push(void *opaque)
{
  svc_ota_t *sa = opaque;
  int r = 1;
  mutex_lock(&sa->sa_mutex);
  pbuf_t *pb = sa->sa_rxbuf;
  if(pb != NULL &&
     pb->pb_offset + pb->pb_buflen + sa->sa_blocksize >= PBUF_DATA_SIZE) {
    r = 0;
  }
  mutex_unlock(&sa->sa_mutex);
  return r;
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
    sa->sa_sock->net->event(sa->sa_sock->net_opaque, PUSHPULL_EVENT_CLOSE);

  pbuf_free(sa->sa_info);
  free(sa);
  ota_busy = 0;
}



static const pushpull_app_fn_t ota_fn = {
  .push = ota_push,
  .may_push = ota_may_push,
  .pull = ota_pull,
  .close = ota_close
};



error_t
ota_open_with_args(pushpull_t *pp,
                   struct block_iface *partition,
                   int xfer_skip_kb,
                   int writeout_skip_kb,
                   int blocksize,
                   void (*platform_upgrade)(uint32_t flow_header,
                                            pbuf_t *pb))
{
  if(ota_busy) {
    return ERR_NOT_READY;
  }

  svc_ota_t *sa = xalloc(sizeof(svc_ota_t), 0,
                         MEM_MAY_FAIL | MEM_TYPE_DMA | MEM_CLEAR);
  if(sa == NULL)
    return ERR_NO_MEMORY;

  sa->sa_partition = partition;
  sa->sa_platform_upgrade = platform_upgrade;
  sa->sa_blocksize = blocksize ?: pp->max_fragment_size;
  sa->sa_skipped_kbytes = writeout_skip_kb;

  pbuf_t *pb = pbuf_make(0, 0);
  if(pb != NULL) {
    uint8_t hdr[4] = {0, 'r', sa->sa_blocksize, xfer_skip_kb};

    pb = pbuf_write(pb, hdr, sizeof(hdr), pp);
    pb = pbuf_write(pb, mios_build_id(), 20, pp);
    const char *appname = mios_get_app_name();
    pb = pbuf_write(pb, appname, strlen(appname), pp);
  }

  if(pb == NULL) {
    free(sa);
    return ERR_NO_BUFFER;
  }

  sa->sa_sock = pp;
  pp->app = &ota_fn;
  pp->app_opaque = sa;

  ota_busy = 1;
  sa->sa_info = pb;
  mutex_init(&sa->sa_mutex, "ota");
  cond_init(&sa->sa_cond, "ota");
  return 0;
}
