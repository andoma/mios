#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/uio.h>

#include <mios/io.h>
#include <mios/task.h>

#include "vi2c.h"

// See vi2c.h

typedef struct vi2c {
  i2c_t i2c;
  mutex_t mutex;
  vi2c_dev_t *devs[128];
  error_t errors[128];
} vi2c_t;


static error_t
vi2c_rwv(struct i2c *bus, uint8_t addr,
         const struct iovec *txiov, const struct iovec *rxiov, size_t count)
{
  vi2c_t *v = (vi2c_t *)bus;

  if(addr >= 128)
    return ERR_INVALID_ARGS;

  mutex_lock(&v->mutex);

  vi2c_dev_t *d = v->devs[addr];
  error_t err = v->errors[addr];

  if(err)
    goto out;

  if(d == NULL) {
    err = ERR_NO_DEVICE;
    goto out;
  }

  if(txiov != NULL || rxiov == NULL) {
    // Write phase, or an address-only probe

    if(d->ops->start(d, addr, 0)) {
      err = ERR_NO_DEVICE;
      goto stop;
    }

    for(size_t i = 0; txiov != NULL && i < count; i++) {
      const uint8_t *b = txiov[i].iov_base;
      for(size_t j = 0; j < txiov[i].iov_len; j++) {
        if(d->ops->write(d, b[j])) {
          err = ERR_BUS_ERROR;
          goto stop;
        }
      }
    }
  }

  if(rxiov != NULL) {

    if(d->ops->start(d, addr, 1)) {
      err = ERR_NO_DEVICE;
      goto stop;
    }

    for(size_t i = 0; i < count; i++) {
      uint8_t *b = rxiov[i].iov_base;
      for(size_t j = 0; j < rxiov[i].iov_len; j++) {
        b[j] = d->ops->read(d);
      }
    }
  }

 stop:
  d->ops->stop(d);
 out:
  mutex_unlock(&v->mutex);
  return err;
}


i2c_t *
vi2c_create(const char *name)
{
  vi2c_t *v = calloc(1, sizeof(vi2c_t));
  mutex_init(&v->mutex, name);
  v->i2c.rwv = vi2c_rwv;
  return &v->i2c;
}


error_t
vi2c_attach(i2c_t *bus, uint8_t addr, vi2c_dev_t *dev)
{
  vi2c_t *v = (vi2c_t *)bus;
  if(addr >= 128)
    return ERR_INVALID_ARGS;

  error_t err = 0;
  mutex_lock(&v->mutex);
  if(v->devs[addr] != NULL)
    err = ERR_EXIST;
  else
    v->devs[addr] = dev;
  mutex_unlock(&v->mutex);
  return err;
}


void
vi2c_detach(i2c_t *bus, uint8_t addr)
{
  vi2c_t *v = (vi2c_t *)bus;
  if(addr >= 128)
    return;
  mutex_lock(&v->mutex);
  v->devs[addr] = NULL;
  mutex_unlock(&v->mutex);
}


void
vi2c_set_error(i2c_t *bus, uint8_t addr, error_t err)
{
  vi2c_t *v = (vi2c_t *)bus;
  if(addr >= 128)
    return;
  mutex_lock(&v->mutex);
  v->errors[addr] = err;
  mutex_unlock(&v->mutex);
}


void
vi2c_lock(i2c_t *bus, int acquire)
{
  vi2c_t *v = (vi2c_t *)bus;
  if(acquire)
    mutex_lock(&v->mutex);
  else
    mutex_unlock(&v->mutex);
}
