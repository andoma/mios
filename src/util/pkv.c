#include <mios/pkv.h>

#include <sys/param.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mios/flash.h>
#include <mios/task.h>

#include "crc32.h"

static mutex_t g_pkv_mutex = MUTEX_INITIALIZER("gpkv");
static pkv_t *g_pkv;

struct pkv {
  const struct flash_iface *fif;
  int sector_a;
  int sector_b;
  size_t sector_size;
  int active_sector;
  int active_version;
  mutex_t mutex;
};

typedef struct sector_header {
  uint8_t magic[4];
  uint32_t version;
  uint32_t crc;
} sector_header_t;

typedef struct kv_header {
  uint8_t marker;
  uint8_t keysize;
  uint8_t valuesize;
} kv_header_t;

#define MARKER_ERASED 0
#define MARKER_BINARY 1
#define MARKER_INT    2

#define MARKED_EMPTY 0xff


static struct pkv *
pkv_obtain(struct pkv *p)
{
  if(p)
    return p;
  return pkv_obtain_global();
}

static int
get_sector_version(struct pkv *pkv, int sector)
{
  sector_header_t hdr;

  error_t e = pkv->fif->read(pkv->fif, sector, 0, &hdr, sizeof(hdr));
  if(e)
    return -1;

  if(memcmp(hdr.magic, "PKV0", 4))
    return -1;

  if(~crc32(0, &hdr, sizeof(hdr)))
    return -1;

  return hdr.version;
}


static error_t
write_sector_header(struct pkv *pkv, int sector, int version)
{
  sector_header_t hdr = {"PKV0"};
  hdr.version = version;

  hdr.crc = ~crc32(0, &hdr, sizeof(hdr) - 4);

  return pkv->fif->write(pkv->fif, sector, 0, &hdr, sizeof(hdr));
}


static error_t
pkv_get_locked(struct pkv *pkv, const char *key, void *buf, size_t *len)
{
  const size_t keysize = strlen(key);

  size_t offset = 12;
  while(1) {
    kv_header_t kvh;

    error_t err = pkv->fif->read(pkv->fif, pkv->active_sector,
                                 offset, &kvh, sizeof(kvh));
    if(err)
      return err;

    if(kvh.marker == 0xff)
      break;

    offset += sizeof(kv_header_t);

    if(kvh.marker > 0 && kvh.keysize == keysize) {
      err = pkv->fif->compare(pkv->fif, pkv->active_sector,
                              offset, key, keysize);
      if(err != ERR_MISMATCH) {

        if(err)
          return err;

        if(*len < kvh.valuesize && buf != NULL) {
          *len = kvh.valuesize;
          return ERR_NO_BUFFER;
        }

        *len = kvh.valuesize;
        if(buf) {
          err = pkv->fif->read(pkv->fif, pkv->active_sector,
                               offset + keysize, buf, kvh.valuesize);

          if(!err) {
            uint32_t stored_crc;
            err = pkv->fif->read(pkv->fif, pkv->active_sector,
                                 offset + keysize + kvh.valuesize,
                                 &stored_crc, sizeof(stored_crc));

            if(!err) {
              uint32_t crc = crc32(0, &kvh, sizeof(kvh));
              crc = crc32(crc, key, keysize);
              crc = ~crc32(crc, buf, kvh.valuesize);
              if(crc != stored_crc) {
                err = ERR_CHECKSUM_ERROR;
              }
            }
          }
        }
        return err;
      }
    }

    offset += kvh.keysize + kvh.valuesize + sizeof(uint32_t) /* CRC */;
  }
  return ERR_NOT_FOUND;
}


error_t
pkv_get(struct pkv *pkv, const char *key, void *buf, size_t *len)
{
  pkv = pkv_obtain(pkv);
  if(pkv == NULL)
    return ERR_NO_DEVICE;

  mutex_lock(&pkv->mutex);
  const error_t err = pkv_get_locked(pkv, key, buf, len);
  mutex_unlock(&pkv->mutex);
  return err;
}


error_t
erase_pkv(struct pkv *pkv, int offset)
{
  uint8_t erased = 0;
  return pkv->fif->write(pkv->fif, pkv->active_sector,
                         offset, &erased, sizeof(erased));
}


error_t
pkv_write(struct pkv *pkv, int offset, int type,
          const char *key, size_t keysize,
          const void *buf, size_t valuesize)
{
  kv_header_t kvh = {type, keysize, valuesize};
  size_t tsize = sizeof(kv_header_t) + keysize + valuesize + sizeof(uint32_t);

  if(offset + tsize >= pkv->sector_size)
    return ERR_NO_BUFFER;

  error_t err;
  err = pkv->fif->write(pkv->fif, pkv->active_sector,
                        offset + sizeof(kv_header_t),
                        key, keysize);

  if(!err)
    err = pkv->fif->write(pkv->fif, pkv->active_sector,
                          offset + sizeof(kv_header_t) + keysize,
                          buf, valuesize);

  if(!err)
    err = pkv->fif->write(pkv->fif, pkv->active_sector,
                          offset,
                          &kvh, sizeof(kvh));

  if(!err) {
    uint32_t crc = crc32(0, &kvh, 3);
    crc = crc32(crc, key, keysize);
    crc = ~crc32(crc, buf, valuesize);

    err = pkv->fif->write(pkv->fif, pkv->active_sector,
                          offset + sizeof(kv_header_t) + keysize + valuesize,
                          &crc, sizeof(crc));
  }

  return err;
}


error_t
pkv_set_locked(struct pkv *pkv, int type,
               const char *key, const void *buf, size_t len)
{
  const size_t keysize = strlen(key);

  error_t err;
  size_t offset = 12;
  size_t old_offset = 0;

  while(1) {
    kv_header_t kvh;

    err = pkv->fif->read(pkv->fif, pkv->active_sector,
                         offset, &kvh, sizeof(kvh));
    if(err)
      return err;

    if(kvh.marker == 0xff)
      break;

    if(kvh.marker > 0 && kvh.keysize == keysize) {
      err = pkv->fif->compare(pkv->fif, pkv->active_sector,
                              offset + sizeof(kv_header_t), key, keysize);

      if(err != ERR_MISMATCH) {

        if(err)
          return err;

        if(old_offset) {
          err = erase_pkv(pkv, old_offset);
          if(err)
            return err;
        }
        old_offset = offset;
      }
    }

    offset += sizeof(kv_header_t) + kvh.keysize + kvh.valuesize + sizeof(uint32_t) /* CRC */;
  }

  if(buf != NULL) {
    err = pkv_write(pkv, offset, type, key, keysize, buf, len);
    if(err)
      return err;
  }

  if(old_offset) {
    err = erase_pkv(pkv, old_offset);
    if(err)
      return err;
  }

  return 0;
}


static error_t
pkv_gc_locked(pkv_t *pkv, const void *skip_key, size_t skip_keysize)
{
  error_t err = 0;

  size_t src_offset = 12;
  size_t dst_offset = 12;

  size_t buflen = 256;
  char *buf = malloc(buflen);

  int next_sector;

  if(pkv->sector_a == pkv->active_sector) {
    next_sector = pkv->sector_b;
  } else {
    next_sector = pkv->sector_a;
  }

  err = pkv->fif->erase_sector(pkv->fif, next_sector);
  while(!err) {
    kv_header_t kvh;

    err = pkv->fif->read(pkv->fif, pkv->active_sector,
                         src_offset, &kvh, sizeof(kvh));
    if(err)
      break;

    if(kvh.marker == 0xff)
      break;

    if(kvh.marker > 0) {

      err = pkv->fif->read(pkv->fif, pkv->active_sector,
                           src_offset + sizeof(kv_header_t), buf, kvh.keysize);
      if(err)
        break;

      if(skip_key == NULL || kvh.keysize != skip_keysize ||
         memcmp(skip_key, buf, kvh.keysize)) {

        uint32_t crc = crc32(0, &kvh, 3);
        crc = crc32(crc, buf, kvh.keysize);

        err = pkv->fif->write(pkv->fif, next_sector,
                              dst_offset + sizeof(kv_header_t),
                              buf, kvh.keysize);
        if(err)
          break;


        err = pkv->fif->read(pkv->fif, pkv->active_sector,
                             src_offset + sizeof(kv_header_t) + kvh.keysize,
                             buf, kvh.valuesize);
        if(err)
          break;

        crc = ~crc32(crc, buf, kvh.valuesize);

        err = pkv->fif->write(pkv->fif, next_sector,
                              dst_offset + sizeof(kv_header_t) + kvh.keysize,
                              buf, kvh.valuesize);
        if(err)
          break;

        err = pkv->fif->write(pkv->fif, pkv->active_sector,
                              dst_offset + sizeof(kv_header_t) + kvh.keysize + kvh.valuesize,
                              &crc, sizeof(crc));
        if(err)
          break;

        err = pkv->fif->write(pkv->fif, next_sector,
                              dst_offset,
                              &kvh, 3);
        if(err)
          break;

        dst_offset += sizeof(kv_header_t) + kvh.keysize + kvh.valuesize + sizeof(uint32_t);
      }
    }

    src_offset += sizeof(kv_header_t) + kvh.keysize + kvh.valuesize + sizeof(uint32_t);
  }

  if(!err) {
    pkv->active_version++;
    pkv->active_sector = next_sector;
    err = write_sector_header(pkv, pkv->active_sector, pkv->active_version);
  }

  free(buf);
  return err;
}




static error_t
pkv_set_typed(struct pkv *pkv, int type,
              const char *key, const void *buf, size_t len)
{
  pkv = pkv_obtain(pkv);

  if(pkv == NULL)
    return ERR_NO_DEVICE;

  mutex_lock(&pkv->mutex);

  error_t err = pkv_set_locked(pkv, type, key, buf, len);
  if(err == ERR_NO_BUFFER) {
    // Out of space, try to GC
    err = pkv_gc_locked(pkv, key, strlen(key));
    if(!err)
      err = pkv_set_locked(pkv, type, key, buf, len);
  }
  mutex_unlock(&pkv->mutex);
  return err;
}



error_t
pkv_set(struct pkv *pkv, const char *key, const void *buf, size_t len)
{
  return pkv_set_typed(pkv, MARKER_BINARY, key, buf, len);
}


error_t
pkv_set_int(struct pkv *pkv, const char *key, int value)
{
  return pkv_set_typed(pkv, MARKER_INT, key, &value, sizeof(value));
}


int
pkv_get_int(struct pkv *pkv, const char *key, int default_value)
{
  int v;
  size_t len = sizeof(v);
  error_t err = pkv_get(pkv, key, &v, &len);

  if(err || len != sizeof(v))
    return default_value;
  return v;
}


error_t
pkv_gc(struct pkv *pkv)
{
  pkv = pkv_obtain(pkv);

  if(pkv == NULL)
    return ERR_NO_DEVICE;

  mutex_lock(&pkv->mutex);
  const error_t err = pkv_gc_locked(pkv, NULL, 0);
  mutex_unlock(&pkv->mutex);
  return err;
}


struct pkv *
pkv_create(const struct flash_iface *fif, int sector_a, int sector_b)
{
  const size_t size_a = fif->get_sector_size(fif, sector_a);
  const size_t size_b = fif->get_sector_size(fif, sector_b);
  if(size_a == 0 || size_b != size_a) {
    printf("pkv: Flash sector size mismatch a(%d) = %d  b(%d) = %d\n",
           sector_a, size_a, sector_b, size_b);
    return NULL;
  }

  struct pkv *pkv = malloc(sizeof(struct pkv));

  mutex_init(&pkv->mutex, "pkv");

  pkv->fif = fif;
  pkv->sector_a = sector_a;
  pkv->sector_b = sector_b;
  pkv->sector_size = size_a;

  const int version_a = get_sector_version(pkv, sector_a);
  const int version_b = get_sector_version(pkv, sector_b);

  if(version_b > version_a) {
    pkv->active_sector = sector_b;
    pkv->active_version = version_b;
  } else {
    pkv->active_sector = sector_a;
    pkv->active_version = version_a;
  }

  if(pkv->active_version == -1) {

    error_t err = pkv->fif->erase_sector(pkv->fif, pkv->active_sector);

    if(!err)
      err = write_sector_header(pkv, pkv->active_sector, 0);

    if(err) {
      printf("pkv: Unable to prime sector %d : %d\n",
             pkv->active_sector, err);
      free(pkv);
      return NULL;
    }
    pkv->active_version = 0;
  }
  return pkv;
}


struct pkv *
pkv_get_global0(void)
{
  if(g_pkv)
    return g_pkv;

  const struct flash_iface *fif = flash_get_primary();
  if(fif == NULL)
    return NULL;

  int s = 0;
  int sectors[2];

  for(int i = 0; i < 2; i++) {
    while(1) {
      flash_sector_type_t t = fif->get_sector_type(fif, s);
      if(t == 0)
        return NULL;
      s++;
      if(t == FLASH_SECTOR_TYPE_PKV) {
        sectors[i] = s - 1;
        break;
      }
    }
  }

  g_pkv = pkv_create(fif, sectors[0], sectors[1]);
  return g_pkv;
}


struct pkv *
pkv_obtain_global(void)
{
  mutex_lock(&g_pkv_mutex);
  struct pkv *r = pkv_get_global0();
  mutex_unlock(&g_pkv_mutex);
  return r;
}


void
pkv_show(struct pkv *pkv, stream_t *out)
{
  pkv = pkv_obtain(pkv);
  if(pkv == NULL)
    return;

  size_t buflen = 256;
  char *buf = malloc(buflen);
  int val32;

  mutex_lock(&pkv->mutex);

  stprintf(out, "Active Sector: %d  Active Version: %d\n",
           pkv->active_sector, pkv->active_version);

  size_t offset = 12;
  while(1) {
    kv_header_t kvh;

    error_t err = pkv->fif->read(pkv->fif, pkv->active_sector,
                                 offset, &kvh, sizeof(kvh));
    if(err) {
      stprintf(out, "Read error %d", err);
      break;
    }

    if(kvh.marker == 0xff)
      break;

    if(kvh.marker > 0) {

      err = pkv->fif->read(pkv->fif, pkv->active_sector,
                           offset + sizeof(kv_header_t), buf, kvh.keysize);
      if(err) {
        stprintf(out, "Read error %d", err);
        break;
      }

      buf[kvh.keysize] = 0;

      stprintf(out, "KEY: \"%s\" (at offset %d)\n", buf, offset);

      err = pkv->fif->read(pkv->fif, pkv->active_sector,
                           offset + sizeof(kv_header_t) + kvh.keysize,
                           buf, kvh.valuesize);
      if(err) {
        stprintf(out, "Read error %d", err);
        break;
      }

      switch(kvh.marker) {
      case MARKER_INT:
        memcpy(&val32, buf, 4);
        stprintf(out, "VAL=%d\n", val32);
        break;
      default:
        sthexdump(out, "VAL", buf, kvh.valuesize, 0);
        break;
      }

    }

    offset += sizeof(kv_header_t) + kvh.keysize + kvh.valuesize + sizeof(uint32_t) /* CRC */;
  }
  mutex_unlock(&pkv->mutex);
  free(buf);
}
